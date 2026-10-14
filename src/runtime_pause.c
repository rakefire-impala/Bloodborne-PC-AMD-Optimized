/* SPDX-License-Identifier: GPL-2.0-or-later
 * Safe menu (launcher: Quality of Life, BB_SAFE_MENU=1): while the start menu is open in single
 * player, enemies and projectiles stand still; the player and the menus run as usual.
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
 * behaviour, so the menu cannot be used to escape them.
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
        {BULLET_UPDATE, 17, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                             0x48,0x83,0xec,0x28}},
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
    game = image;
    active = 1;
    puts("Safe menu: verified 1.09; enemies and projectiles stop while the start menu is open");
}

void runtime_pause_poll(void) {
    if (!active) return;
    if (in_game() && start_menu_open() && single_player()) {
        each_enemy(freeze);
        paused = 1;
    } else if (paused || frozen_count) {
        each_enemy(thaw);
        frozen_count = 0;
        paused = 0;
    }
}
