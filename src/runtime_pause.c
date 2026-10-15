/* SPDX-License-Identifier: GPL-2.0-or-later
 * Safe menu (launcher: Quality of Life, BB_SAFE_MENU=1): while the start menu is open in single
 * player, enemies, projectiles, the world's sounds and its effects (fire, fog, ...) stand still;
 * the player and the menus (with their sounds) run as usual.
 *
 * Enemies use the game's own debug freeze ("Switch All No Update", 1.09 image 0x19230b0): bit
 * 0x400 of each character's debug flags (ChrIns +0x3b0 -> +0x20 -> word +0x200) makes the
 * character update skip it. Like the debug switch, every character set but the last two (the
 * players') is frozen. Only characters frozen here are thawed, so the debug menu's own freezes
 * stay. Projectiles: three calls return at once while paused. SprjBulletManager's step (image
 * 0x1a011a0: spawn requests, then each bullet's update and flight) from one world task; from
 * another (0x19381e0), the attack manager's update (hit objects: lifetime and hits of bullets
 * and attacks) and SprjBulletManager's update (creation phases, emitters). A projectile fired
 * as the menu opens resumes its flight afterwards; nothing moves, hits or expires meanwhile.
 * The player is not made invulnerable: falls, poison and the like keep the game's own
 * behaviour, so the menu cannot be used to escape them. Effects: the scene's update (0x22ff340)
 * has an unused switch (+0x5ce) that gives everything after it, the particle effects among
 * them, no time; they keep being drawn as they are. Sounds: see sound_update.
 *
 * Polled on the thread that reads the pad, once per read. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum {
    WORLD_CHR_MAN = 0x553e878,   /* WorldChrMan *: +0x60 local player, +0x278 set count, sets at +0x280 */
    MENU_MAN = 0x5562878,        /* menu manager *: +0x54 start menu state (2 = open) */
    SESSION_MAN = 0x5540290,     /* SprjSessionManager *: +0x124 session state */
    STEP_CALL = 0x193bb49,       /* in a world task: call SprjBulletManager's step */
    STEP_UPDATE = 0x1a011a0,
    ATTACK_CALL = 0x1938246,     /* in another world task: call the attack manager's update */
    ATTACK_UPDATE = 0x1863540,
    BULLET_CALL = 0x19382b0,     /* the task's tail call: jmp SprjBulletManager's update */
    BULLET_UPDATE = 0x1a02c60,
    EVENT_UPDATE = 0xd1e250,     /* FMOD EventSystemI::update(this), once per frame (slot 0x88) */
    GET_CATEGORY = 0xd1caa0,     /* FMOD::EventSystem::getCategory(this, name, EventCategory **) */
    CATEGORY_VTABLE = 0x52d9220, /* EventCategoryI: +0x68 channel group, +0x70 child categories */
    CATEGORY_SET_PAUSED = 0xdd5460, /* EventCategoryI::setPaused: mov rdi,[rbx+0x68] */
    CATEGORY_CHILD = 0xdd4d6a,   /* EventCategoryI::getCategoryByIndex: [rdi+0x70], list at +8 */
    GROUP_SET_PAUSED = 0xd33f80, /* ChannelGroupI::setPaused(this, paused, own): +0xa1, +0x50, +0x58 */
    SCENE = 0x54b0b30,           /* the graphics scene *: +0x5ce effects get no time when set */
    SCENE_VTABLE = 0x53b9080,
    SCENE_STILL = 0x22ff434,     /* scene update: cmp byte [rbx+0x5ce],0; je; vxorps xmm0 (dt = 0) */
    MENU_OPEN = 2,
    NO_UPDATE = 0x400,
    MAX_FROZEN = 4096,
};

static unsigned char *game;
static volatile unsigned char paused; /* read by the hooks */
static int active;
static uintptr_t frozen[MAX_FROZEN];
static int frozen_count;

static uintptr_t load(uintptr_t address) {
    uintptr_t value;
    memcpy(&value, (const void *)address, sizeof(value));
    return value;
}
static int32_t load32(uintptr_t address) {
    int32_t value;
    memcpy(&value, (const void *)address, sizeof(value));
    return value;
}
/* IsHost (3), IsTryJoinSession (4), IsClient (6): the game's own multiplayer states. */
static int single_player(void) {
    const uintptr_t session = load((uintptr_t)game + SESSION_MAN);
    if (!session) return 0;
    const int32_t state = load32(session + 0x124);
    return state != 3 && state != 4 && state != 6;
}
static int in_game(void) {
    const uintptr_t world = load((uintptr_t)game + WORLD_CHR_MAN);
    return world && load(world + 0x60);
}
static int start_menu_open(void) {
    const uintptr_t menu = load((uintptr_t)game + MENU_MAN);
    return menu && load32(menu + 0x54) == MENU_OPEN;
}
static int was_frozen(uintptr_t chr) {
    for (int i = 0; i < frozen_count; ++i) if (frozen[i] == chr) return 1;
    return 0;
}
/* Calls visit(flags word, character) for every character the debug switch would freeze. */
static void each_enemy(void (*visit)(uint16_t *, uintptr_t)) {
    const uintptr_t world = load((uintptr_t)game + WORLD_CHR_MAN);
    if (!world) return;
    const int32_t sets = load32(world + 0x278);
    for (int32_t s = 0; s < sets - 2; ++s) {
        const uintptr_t set = load(world + 0x280 + 0x18 * (uintptr_t)s);
        if (!set) continue;
        const int32_t count = load32(set);
        const uintptr_t entries = load(set + 8);
        if (!entries) continue;
        for (int32_t i = 0; i < count; ++i) {
            const uintptr_t chr = load(entries + 0x38 * (uintptr_t)i);
            if (!chr) continue;
            const uintptr_t modules = load(chr + 0x3b0);
            const uintptr_t data = modules ? load(modules + 0x20) : 0;
            if (data) visit((uint16_t *)(data + 0x200), chr);
        }
    }
}
/* Atomic, so bits the game changes meanwhile are kept; re-applied on every poll in case the
 * game's own read-modify-write of the word drops ours, and for characters loaded meanwhile. */
static void freeze(uint16_t *flags, uintptr_t chr) {
    if (__atomic_load_n(flags, __ATOMIC_RELAXED) & NO_UPDATE) {
        if (!was_frozen(chr)) return; /* frozen by the debug menu: not ours */
    } else if (!was_frozen(chr)) {
        if (frozen_count == MAX_FROZEN) return;
        frozen[frozen_count++] = chr;
    }
    __atomic_fetch_or(flags, (uint16_t)NO_UPDATE, __ATOMIC_RELAXED);
}
/* Only characters still loaded are touched: the list may name freed ones (a Hunter's Mark used
 * from the inventory reloads the area). */
static void thaw(uint16_t *flags, uintptr_t chr) {
    if (was_frozen(chr)) __atomic_fetch_and(flags, (uint16_t)~NO_UPDATE, __ATOMIC_RELAXED);
}

/* A byte the scene update reads once per frame; set only if it was clear, cleared only on the
 * same scene (an area load meanwhile makes a new one). */
static uintptr_t still_scene;
static void pause_effects(void) {
    if (still_scene) return;
    const uintptr_t scene = load((uintptr_t)game + SCENE);
    if (!scene || load(scene) != (uintptr_t)game + SCENE_VTABLE) return;
    volatile unsigned char *still = (volatile unsigned char *)(scene + 0x5ce);
    if (*still) return;
    *still = 1;
    still_scene = scene;
}
static void resume_effects(void) {
    if (!still_scene) return;
    const uintptr_t scene = load((uintptr_t)game + SCENE);
    if (scene == still_scene && load(scene) == (uintptr_t)game + SCENE_VTABLE)
        *(volatile unsigned char *)(scene + 0x5ce) = 0;
    still_scene = 0;
}

/* FMOD's event API is not thread-safe: sounds are paused at the start of EventSystemI::update,
 * on the thread that runs FMOD. Pausing a whole category would also silence the menu's own
 * sounds (they share "SE" with the world), so each category's channel group keeps its own state
 * and only the event groups under it that are playing when the menu opens are paused; events
 * started afterwards (the menu's) play. A channel is paused when its group or any parent is
 * (ChannelGroupI +0xa1), so a paused event group holds all of the event's sounds. */
typedef int32_t (ABI *GetCategory)(void *, const char *, void **);
typedef int32_t (ABI *GroupSetPaused)(uintptr_t, uint32_t, uint32_t);
enum { MAX_CATEGORIES = 64, MAX_SOUNDS = 512 };
static uintptr_t category_groups[MAX_CATEGORIES], sound_groups[MAX_SOUNDS];
static int category_count, sound_count;
static int sound_paused;
static void collect_categories(uintptr_t category) {
    if (category_count == MAX_CATEGORIES || load(category) != (uintptr_t)game + CATEGORY_VTABLE) return;
    const uintptr_t group = load(category + 0x68);
    if (group) category_groups[category_count++] = group;
    const uintptr_t children = load(category + 0x70);
    if (!children) return;
    for (uintptr_t node = load(children + 8), n = 0; node && node != children + 8 && n < MAX_CATEGORIES;
         node = load(node), ++n)
        collect_categories(node - 8);
}
static int is_category_group(uintptr_t group) {
    for (int i = 0; i < category_count; ++i) if (category_groups[i] == group) return 1;
    return 0;
}
/* Whether a channel group or one under it has channels (a playing event). */
static int has_channels(uintptr_t group, int depth) {
    if (load(group + 0x58) != group + 0x58) return 1;
    const uintptr_t head = load(group + 0x50);
    if (!head || depth > 4) return 0;
    for (uintptr_t node = load(head + 8), n = 0; node && node - 8 != head && n < 256; node = load(node), ++n)
        if (has_channels(node - 8, depth + 1)) return 1;
    return 0;
}
/* Calls visit on each event group directly under a category's group. */
static void each_sound(void (*visit)(uintptr_t)) {
    for (int i = 0; i < category_count; ++i) {
        const uintptr_t head = load(category_groups[i] + 0x50);
        if (!head) continue;
        for (uintptr_t node = load(head + 8), n = 0; node && node - 8 != head && n < 1024; node = load(node), ++n)
            if (!is_category_group(node - 8)) visit(node - 8);
    }
}
static void pause_sound(uintptr_t group) {
    if (sound_count == MAX_SOUNDS || *(volatile unsigned char *)(group + 0xa1) || !has_channels(group, 0)) return;
    ((GroupSetPaused)(game + GROUP_SET_PAUSED))(group, 1, 1);
    sound_groups[sound_count++] = group;
}
static void resume_sound(uintptr_t group) {
    for (int i = 0; i < sound_count; ++i)
        if (sound_groups[i] == group) {
            if (*(volatile unsigned char *)(group + 0xa1)) ((GroupSetPaused)(game + GROUP_SET_PAUSED))(group, 0, 1);
            return;
        }
}
static ABI void sound_update(void *system) {
    const int want = paused;
    if (want == sound_paused) return;
    sound_paused = want;
    void *master = NULL;
    category_count = 0;
    if (((GetCategory)(game + GET_CATEGORY))(system, "master", &master) || !master) return;
    collect_categories((uintptr_t)master);
    if (want) each_sound(pause_sound);
    else {
        each_sound(resume_sound);
        sound_count = 0;
    }
}

void runtime_pause_install(void *image, size_t size, const char *app_version) {
    /* The debug switch's loop: set count - 2, set +0x280 stride 0x18, entries stride 0x38,
     * ChrIns +0x3b0 -> +0x20, flags word +0x200 bit 10. */
    static const struct { size_t offset; unsigned char length; unsigned char bytes[22]; } checks[] = {
        {0x1923339, 9, {0x8b,0x88,0x78,0x02,0x00,0x00,0x83,0xc1,0xfe}},
        {0x192339e, 12, {0x48,0x8d,0x0c,0x5b,0x4c,0x8b,0xbc,0xc8,0x80,0x02,0x00,0x00}},
        {0x19233d0, 4, {0x49,0x83,0xc5,0x38}},
        {0x19233db, 8, {0x49,0x8b,0x47,0x08,0x4a,0x8b,0x04,0x28}},
        {0x19233e8, 11, {0x48,0x8b,0x80,0xb0,0x03,0x00,0x00,0x4c,0x8b,0x60,0x20}},
        {0x192342a, 20, {0x41,0x0f,0xb7,0x8c,0x24,0x00,0x02,0x00,0x00,0xc1,0xe0,0x0a,
                         0x81,0xe1,0xff,0xfb,0x00,0x00,0x09,0xc1}},
        /* The game's "close the start menu": mov rax,[menu manager]; cmp [rax+0x54],2. */
        {0x16ac080, 11, {0x48,0x8b,0x05,0xf1,0x67,0xeb,0x03,0x83,0x78,0x54,0x02}},
        /* IsHost: lea rbx,[SprjSessionManager] ... cmp dword [rax+0x124],3. */
        {0x132ace6, 7, {0x48,0x8d,0x1d,0xa3,0x55,0x21,0x04}},
        {0x132ad19, 7, {0x83,0xb8,0x24,0x01,0x00,0x00,0x03}},
        /* ChrIns::IsLocalPlayer compares with WorldChrMan +0x60. */
        {0x18c2d3d, 4, {0x48,0x39,0x58,0x60}},
        /* vmovss xmm0,[rbp-0x12c] (dt); call step. */
        {STEP_CALL - 8, 13, {0xc5,0xfa,0x10,0x85,0xd4,0xfe,0xff,0xff,0xe8,0x52,0x56,0x0c,0x00}},
        {STEP_UPDATE, 20, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                           0x48,0x81,0xec,0x98,0x2c,0x00,0x00}},
        /* lea rax,[attack manager]; mov rdi,[rax]; call update. */
        {ATTACK_CALL - 10, 15, {0x48,0x8d,0x05,0xfd,0x54,0xc0,0x03,0x48,0x8b,0x38,0xe8,0xf5,0xb2,0xf2,0xff}},
        {ATTACK_UPDATE, 17, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                             0x48,0x83,0xec,0x78}},
        {BULLET_CALL, 5, {0xe9,0xab,0xa9,0x0c,0x00}},
        /* push rbp; mov rbp,rsp; push r15..r12; push rbx; sub rsp,0x18; mov r14,rdi */
        {EVENT_UPDATE, 20, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,
                            0x83,0xec,0x18,0x49,0x89,0xfe}},
        {GET_CATEGORY, 16, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xd6}},
        {CATEGORY_SET_PAUSED + 13, 4, {0x48,0x8b,0x7b,0x68}},
        {CATEGORY_CHILD, 4, {0x48,0x8b,0x7f,0x70}},
        {GROUP_SET_PAUSED + 0x27, 19, {0x41,0x88,0xb6,0xa1,0x00,0x00,0x00,0x49,0x8b,0x46,0x50,0x48,0x85,0xc0,0x74,0x51,0x48,0x8b,0x48}},
        {BULLET_UPDATE, 17, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                             0x48,0x83,0xec,0x28}},
        {SCENE_STILL, 13, {0x80,0xbb,0xce,0x05,0x00,0x00,0x00,0x74,0x04,0xc5,0xf8,0x57,0xc0}},
        /* An accessor of the scene: lea r13,[scene]. */
        {0xefcb76, 7, {0x4c,0x8d,0x2d,0xb3,0x3f,0x5b,0x04}},
    };
    const char *enabled = getenv("BB_SAFE_MENU");
    if (!enabled || strcmp(enabled, "1")) return;
    const unsigned char *code = image;
    int ok = app_version && !strcmp(app_version, "01.09") && size > MENU_MAN + 8;
    for (size_t i = 0; ok && i < sizeof(checks) / sizeof(checks[0]); ++i)
        ok = memcmp(code + checks[i].offset, checks[i].bytes, checks[i].length) == 0;
    if (!ok) {
        puts("Safe menu: executable/version guard failed; the start menu does not pause");
        return;
    }
    /* Each site keeps its call or jmp, now to a stub: movabs rax,&paused; cmp byte [rax],0;
     * jne +12; movabs rax,update; jmp rax; ret. RAX holds nothing live at these sites. */
    static const struct { size_t site, target; } hooks[] = {
        {STEP_CALL, STEP_UPDATE}, {ATTACK_CALL, ATTACK_UPDATE}, {BULLET_CALL, BULLET_UPDATE}};
    enum { HOOKS = sizeof(hooks) / sizeof(hooks[0]) };
    unsigned char *page = runtime_low_map(4096, PROT_READ | PROT_WRITE);
    if (!page || page == (void *)-1) return;
    int32_t rel[HOOKS];
    for (int i = 0; i < HOOKS; ++i) {
        unsigned char *stub = page + 32 * i;
        const uintptr_t flag_address = (uintptr_t)&paused, update = (uintptr_t)image + hooks[i].target;
        static const unsigned char code_template[28] = {0x48,0xb8, 0,0,0,0,0,0,0,0, 0x80,0x38,0x00,
            0x75,0x0c, 0x48,0xb8, 0,0,0,0,0,0,0,0, 0xff,0xe0, 0xc3};
        memcpy(stub, code_template, sizeof(code_template));
        memcpy(stub + 2, &flag_address, 8);
        memcpy(stub + 17, &update, 8);
        const int64_t distance = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(code + hooks[i].site + 5);
        if (distance < INT32_MIN || distance > INT32_MAX) return;
        rel[i] = (int32_t)distance;
    }
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) return;
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
#else
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC)) return;
    __builtin___clear_cache((char *)page, (char *)page + 32 * HOOKS);
#endif
    for (int i = 0; i < HOOKS; ++i) memcpy((unsigned char *)image + hooks[i].site + 1, &rel[i], 4);
    /* EventSystemI::update starts with a jump to a stub: push rdi; call sound_update; pop rdi;
     * the 13 bytes it replaced (push rbp ... push rbx); jump back. */
    {
        unsigned char *sound = runtime_low_map(4096, PROT_READ | PROT_WRITE), *e = sound;
        if (!sound || sound == (void *)-1) return;
        const uintptr_t handler = (uintptr_t)sound_update, back = (uintptr_t)image + EVENT_UPDATE + 13;
        *e++ = 0x57; *e++ = 0x48; *e++ = 0xb8; memcpy(e, &handler, 8); e += 8;
        *e++ = 0xff; *e++ = 0xd0; *e++ = 0x5f;
        memcpy(e, code + EVENT_UPDATE, 13); e += 13;
        *e++ = 0x48; *e++ = 0xb8; memcpy(e, &back, 8); e += 8; *e++ = 0xff; *e++ = 0xe0;
#ifdef _WIN32
        if (!VirtualProtect(sound, 4096, PAGE_EXECUTE_READ, &old)) return;
        FlushInstructionCache(GetCurrentProcess(), sound, 4096);
#else
        if (mprotect(sound, 4096, PROT_READ | PROT_EXEC)) return;
#endif
        unsigned char *entry = (unsigned char *)image + EVENT_UPDATE;
        const uintptr_t to = (uintptr_t)sound;
        entry[0] = 0x48; entry[1] = 0xb8; memcpy(entry + 2, &to, 8); entry[10] = 0xff; entry[11] = 0xe0;
        entry[12] = 0x90;
    }
    game = image;
    active = 1;
    puts("Safe menu: verified 1.09; enemies, projectiles, sounds and effects stop while the start menu is open");
}

void runtime_pause_poll(void) {
    if (!active) return;
    if (in_game() && start_menu_open() && single_player()) {
        each_enemy(freeze);
        pause_effects();
        paused = 1;
    } else if (paused || frozen_count) {
        each_enemy(thaw);
        resume_effects();
        frozen_count = 0;
        paused = 0;
    }
}
