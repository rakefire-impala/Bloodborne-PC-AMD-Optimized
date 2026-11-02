/* SPDX-License-Identifier: GPL-2.0-or-later
 * Boss retry (launcher: Quality of Life). After a death in a major boss fight, the overlay asks
 * whether to retry it before the game reloads: the reload waits for the answer. Retrying puts the
 * player at the boss's fog gate after that one loading screen; declining respawns as usual. Polled
 * on the thread that reads the pad, once per read.
 *
 * The generated event scripts (scripts/boss_retry.py, list in BB_BOSS_RETRY_LIST) do the game
 * side through event flags: walking into a boss's fog writes the gate's number to BITS and sets
 * ENTERED; REQUEST with a gate's number in BITS makes the reload arrive at it. A fight is a boss
 * bar of a major boss (a name in the list, not an NPC hunter) appearing after the player walked
 * into a gate whose boss lives; a death is the player's HP at 0 during it. Boss bars are read as
 * the HUD does (see runtime_discord.c): menu manager +0x1658, 3 slots of {name ID, character
 * handle}.
 *
 * The game's solo death is a chain of named steps on its event timer (registered at 0x138db88):
 * SoloPlayDeath, SoloPlayDeath_TextWait and, as YOU DIED ends (about 9 s after the death),
 * SoloPlayDeath_2, which requests the reload. With a retry to offer, that last step returns
 * without running; the answer puts it back on the timer, as SoloPlayDeath_1 schedules it. (Not
 * from inside the step: it would fire again at once, over and over, until the game's heap runs
 * out. It runs on this thread.) */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum {
    IS_EVENT_FLAG = 0x1399bf0,   /* bool (ignored, flag id): the "IsEventFlag" binding's target */
    SET_EVENT_FLAG = 0x13cfcc0,  /* (SprjEventFlagMan *, flag id, value) */
    FLAGS_READY = 0x553b0d8,     /* non-null once the game has event flags */
    EVENT_FLAG_MAN = 0x553b100,  /* SprjEventFlagMan singleton */
    WORLD_CHR_MAN = 0x553e878,   /* WorldChrMan *: +0x60 local player, +0x850 character sets */
    MENU_MAN = 0x5562878,        /* menu manager *: +0x54 start menu state, boss bars at +0x1658 */
    SESSION_MAN = 0x5540290,     /* SprjSessionManager *: +0x124 session state */
    BLOCK_LIST = 0x553b148,      /* +0x20 index of the player's block, +0x10 -> +0x18 count, +0x20 entries */
    BOSS_BARS = 0x1658,
    DEATH_RELOAD = 0x1381de0,    /* the step SoloPlayDeath_2 (arguments: ?, the event proxy) */
    DEATH_RELOAD_NAME = 0x492e368,
    SCHEDULE_STEP = 0x132b1c0,   /* (proxy, event id, step name, 0, 3, 1, delay in s) */
    DEATH_EVENT = 4000,
    PROXY_SCHEDULING = 0x8a,     /* proxy: byte the game clears around SCHEDULE_STEP */
    STEP_PROLOGUE = 13,          /* push rbp ... mov r14,rdi: moved to the stub */
    MAX_GATES = 256,
    MAX_NAMES = 256,
    /* Pad reads (about one per frame) a fight's bar may stay away before the fight counts as over,
     * and the event script may take to take a request (the world stops without focus). */
    BAR_GONE_READS = 180,
    REQUEST_READS = 300,
};
typedef int (ABI *IsEventFlag)(void *, uint32_t);
typedef void (ABI *SetEventFlag)(void *, uint32_t, uint32_t);
typedef void (ABI *ScheduleStep)(void *, uint32_t, const char *, uint32_t, uint32_t, uint32_t, float);

static unsigned char *game;
static int active;
static int request_flag, entered_flag, bits_flag, bit_count;
static struct { int defeat_flag, area, block, lamp; } gates[MAX_GATES];
static int gate_count;
static struct { int id; char name[96]; } names[MAX_NAMES];
static int name_count;
static char question[128], retry_text[64], decline_text[64];
/* The gate the player last walked into, the fight (gate and boss name), the retry offered and
 * the event proxy of the reload held for it. */
static int entered = -1, fight = -1, fight_name, pending = -1, pending_name, shown;
static unsigned char *held_reload;
static unsigned bar_gone, request_reads;

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
static int pointer_ok(uintptr_t p) { return p >= 65536; }
static int ready(void) {
    return pointer_ok(load((uintptr_t)game + EVENT_FLAG_MAN)) && pointer_ok(load((uintptr_t)game + FLAGS_READY));
}
static int get(int flag) {
    return ((IsEventFlag)(game + IS_EVENT_FLAG))(NULL, (uint32_t)flag) & 1;
}
static void set(int flag, int value) {
    ((SetEventFlag)(game + SET_EVENT_FLAG))((void *)load((uintptr_t)game + EVENT_FLAG_MAN), (uint32_t)flag,
                                            (uint32_t)value);
}
static const char *boss_name(int id) {
    for (int i = 0; i < name_count; ++i) if (names[i].id == id) return names[i].name;
    return NULL;
}
/* IsHost (3), IsTryJoinSession (4), IsClient (6): the game's own multiplayer states. */
static int single_player(void) {
    const uintptr_t session = load((uintptr_t)game + SESSION_MAN);
    if (!pointer_ok(session)) return 0;
    const int32_t state = load32(session + 0x124);
    return state != 3 && state != 4 && state != 6;
}
static int start_menu_open(void) {
    const uintptr_t menu = load((uintptr_t)game + MENU_MAN);
    return pointer_ok(menu) && load32(menu + 0x54) != 0;
}
/* HP of a character (ChrIns +0x3b0 -> +0x20 -> +0xf8), -1 when unreadable. */
static int32_t hp(uintptr_t chr) {
    const uintptr_t modules = pointer_ok(chr) ? load(chr + 0x3b0) : 0;
    const uintptr_t stats = pointer_ok(modules) ? load(modules + 0x20) : 0;
    return pointer_ok(stats) ? load32(stats + 0xf8) : -1;
}
/* The block the player is in (0xAABBCCDD for mAA_BB_CC_DD), as GetLastBlockId reads it. */
static int player_block(int32_t *id) {
    const uintptr_t list = load((uintptr_t)game + BLOCK_LIST);
    if (!pointer_ok(list)) return 0;
    const int32_t index = load32(list + 0x20);
    const uintptr_t blocks = load(list + 0x10);
    if (index < 0 || !pointer_ok(blocks) || index >= load32(blocks + 0x18)) return 0;
    const uintptr_t entries = load(blocks + 0x20);
    if (!pointer_ok(entries)) return 0;
    *id = load32(entries + (uintptr_t)index * 0xa0 + 8);
    return *id != -1;
}
/* The first boss bar of a major boss whose character still has HP; its name ID, or -1. */
static int major_bar(uintptr_t world) {
    const uintptr_t menu = load((uintptr_t)game + MENU_MAN);
    const uintptr_t info = load(world + 8);
    if (!pointer_ok(menu) || !pointer_ok(info)) return -1;
    const int32_t sets = load32(info + 0x18) + 2;
    for (int slot = 0; slot < 3; ++slot) {
        const int32_t name = load32(menu + BOSS_BARS + 20 * slot);
        const uint32_t handle = (uint32_t)load32(menu + BOSS_BARS + 20 * slot + 4);
        if (name < 0 || handle == 0xffffffffu || !boss_name(name)) continue;
        const int32_t set = (int32_t)((handle >> 14) & 0x3f), entry = (int32_t)(handle & 0x3fff);
        if (set >= sets) continue;
        const uintptr_t characters = load(world + 0x850 + 8 * (uintptr_t)set);
        if (!pointer_ok(characters) || entry >= load32(characters)) continue;
        const uintptr_t entries = load(characters + 8);
        if (pointer_ok(entries) && hp(load(entries + (uintptr_t)entry * 0x38)) > 0) return name;
    }
    return -1;
}

/* Entered at SoloPlayDeath_2: nonzero skips the step, holding the reload. */
static int ABI death_reload_step(void *unused, unsigned char *proxy) {
    (void)unused;
    if (pending < 0) return 0;
    held_reload = proxy;
    return 1;
}
/* No retry to offer any more: the reload held for it, if any, goes on. */
static void end_offer(void) {
    pending = -1;
    if (!held_reload) return;
    unsigned char *proxy = held_reload;
    held_reload = NULL;
    proxy[PROXY_SCHEDULING] = 0;
    ((ScheduleStep)(game + SCHEDULE_STEP))(proxy, DEATH_EVENT, (const char *)game + DEATH_RELOAD_NAME, 0, 3, 1, 0.1f);
    proxy[PROXY_SCHEDULING] = 1;
}
/* The step starts with a jump to a stub: push rdi; push rsi; sub rsp,8; call death_reload_step;
 * add rsp,8; pop rsi; pop rdi; test eax,eax; je +1; ret; the bytes it replaced; jump back. RAX is
 * written before it is read after them. */
static int hook_death_reload(unsigned char *image) {
    unsigned char *page = runtime_low_map(4096, PROT_READ | PROT_WRITE), *e = page;
    if (!page || page == (void *)-1) return 0;
    const uintptr_t handler = (uintptr_t)death_reload_step, back = (uintptr_t)image + DEATH_RELOAD + STEP_PROLOGUE;
    unsigned char *entry = image + DEATH_RELOAD;
    const int64_t distance = (int64_t)(uintptr_t)page - (int64_t)(uintptr_t)(entry + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) return 0;
    *e++ = 0x57; *e++ = 0x56; *e++ = 0x48; *e++ = 0x83; *e++ = 0xec; *e++ = 0x08;
    *e++ = 0x48; *e++ = 0xb8; memcpy(e, &handler, 8); e += 8; *e++ = 0xff; *e++ = 0xd0;
    *e++ = 0x48; *e++ = 0x83; *e++ = 0xc4; *e++ = 0x08; *e++ = 0x5e; *e++ = 0x5f;
    *e++ = 0x85; *e++ = 0xc0; *e++ = 0x74; *e++ = 0x01; *e++ = 0xc3;
    memcpy(e, entry, STEP_PROLOGUE); e += STEP_PROLOGUE;
    *e++ = 0x48; *e++ = 0xb8; memcpy(e, &back, 8); e += 8; *e++ = 0xff; *e++ = 0xe0;
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) return 0;
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
#else
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC)) return 0;
    __builtin___clear_cache((char *)page, (char *)e);
#endif
    const int32_t rel32 = (int32_t)distance;
    entry[0] = 0xe9;
    memcpy(entry + 1, &rel32, 4);
    return 1;
}

static int load_list(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *field[8];
        int n = 0;
        for (char *p = line; n < 8;) {
            field[n++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = '\0';
            p = tab + 1;
        }
        if (!strcmp(field[0], "flags") && n >= 5) {
            request_flag = atoi(field[1]);
            entered_flag = atoi(field[2]);
            bits_flag = atoi(field[3]);
            bit_count = atoi(field[4]);
        } else if (!strcmp(field[0], "texts") && n >= 4) {
            snprintf(question, sizeof(question), "%s", field[1]);
            snprintf(retry_text, sizeof(retry_text), "%s", field[2]);
            snprintf(decline_text, sizeof(decline_text), "%s", field[3]);
        } else if (!strcmp(field[0], "gate") && n >= 6 && gate_count < MAX_GATES && atoi(field[1]) == gate_count) {
            gates[gate_count].defeat_flag = atoi(field[2]);
            gates[gate_count].area = atoi(field[3]);
            gates[gate_count].block = atoi(field[4]);
            gates[gate_count].lamp = atoi(field[5]);
            ++gate_count;
        } else if (!strcmp(field[0], "boss") && n >= 3 && name_count < MAX_NAMES) {
            names[name_count].id = atoi(field[1]);
            snprintf(names[name_count].name, sizeof(names[name_count].name), "%s", field[2]);
            ++name_count;
        }
    }
    fclose(f);
    return request_flag && entered_flag && bits_flag && bit_count > 0 && bit_count <= 16 && gate_count > 0 &&
           name_count > 0 && question[0];
}

void runtime_retry_install(void *image, size_t size, const char *app_version) {
    /* IsEventFlag, SetEventFlag (as runtime_lamp.c checks them) and the boss bar setter and HUD
     * (as runtime_discord.c checks them). */
    static const struct { size_t offset; unsigned char length; unsigned char bytes[52]; } checks[] = {
        {IS_EVENT_FLAG, 20, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x41,0x89,0xf7,0x4c,0x8d,0x35,
                             0xfc,0x14,0x1a,0x04}},
        {SET_EVENT_FLAG, 19, {0x41,0x89,0xd0,0x8b,0x4f,0x1c,0x31,0xd2,0x89,0xf0,0xf7,0xf1,0x41,0x89,0xc1,0x44,
                              0x0f,0xaf,0xc9}},
        {0x131ef3f, 52, {0x83,0xfb,0x02,0x77,0x2f,0x4c,0x8b,0x3d,0x2d,0x39,0x24,0x04,0x31,0xd2,0x89,0xf7,0xe8,
                         0x4c,0xa8,0x0a,0x00,0x48,0x85,0xc0,0x74,0x1a,0x48,0x63,0xcb,0x8b,0x40,0x08,0x48,0x8d,
                         0x0c,0x89,0x41,0x89,0x84,0x8f,0x5c,0x16,0x00,0x00,0x45,0x89,0xb4,0x8f,0x58,0x16,0x00,
                         0x00}},
        {0x164c427, 31, {0x48,0x8b,0x00,0x48,0x85,0xc0,0x0f,0x84,0xb9,0x02,0x00,0x00,0x48,0x8b,0x80,0xb0,0x03,
                         0x00,0x00,0x48,0x8b,0x40,0x20,0xc5,0xfa,0x2a,0x80,0xf8,0x00,0x00,0x00}},
        /* GetLastBlockId: lea rcx,[block list]; ... movsxd rcx,[rdx+0x20]. */
        {0x1332db8, 19, {0x48,0x8d,0x0d,0x89,0x83,0x20,0x04,0x48,0x8b,0x11,0x48,0x85,0xd2,0x74,0x31,0x48,0x63,
                         0x4a,0x20}},
        /* SoloPlayDeath_1 scheduling SoloPlayDeath_2: lea rdx,[name] ... call schedule. */
        {0x1381da0, 7, {0x48,0x8d,0x15,0xc1,0xc5,0x5a,0x03}},
        {0x1381dc5, 5, {0xe8,0xf6,0x93,0xfa,0xff}},
        /* The step's start and the name. */
        {DEATH_RELOAD, STEP_PROLOGUE, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xfe}},
        {DEATH_RELOAD_NAME, 16, {'S','o','l','o','P','l','a','y','D','e','a','t','h','_','2',0}},
    };
    const char *list = getenv("BB_BOSS_RETRY_LIST");
    if (!list || !*list) return;
    const unsigned char *code = image;
    int ok = app_version && !strcmp(app_version, "01.09") && size > MENU_MAN + 8;
    for (size_t i = 0; ok && i < sizeof(checks) / sizeof(checks[0]); ++i)
        ok = memcmp(code + checks[i].offset, checks[i].bytes, checks[i].length) == 0;
    if (!ok) {
        puts("Boss retry: executable/version guard failed; no retries");
        return;
    }
    if (!load_list(list)) {
        printf("Boss retry: cannot read %s; no retries\n", list);
        return;
    }
    game = image;
    if (!hook_death_reload(image)) {
        puts("Boss retry: cannot hook the death reload; no retries");
        return;
    }
    active = 1;
    printf("Boss retry: %d boss fog gates, %d boss names\n", gate_count, name_count);
}

void runtime_retry_poll(void) {
    if (!active || !ready()) return;
    if (shown) {
        const int answer = bbgpu_retry_prompt_result();
        if (answer == BBGPU_RETRY_PENDING) return;
        shown = 0;
        if (answer == 1) {
            for (int k = 0; k < bit_count; ++k) set(bits_flag + k, pending >> k & 1);
            set(request_flag, 1);
            request_reads = 1;
            printf("Boss retry: retrying gate %d\n", pending);
            return;
        }
        puts("Boss retry: respawning");
        end_offer();
        return;
    }
    /* The reload waits until the event script has taken the request. */
    if (request_reads) {
        if (get(request_flag) && ++request_reads <= REQUEST_READS) return;
        if (get(request_flag)) {
            set(request_flag, 0);
            puts("Boss retry: the request was not taken; respawning as usual");
        }
        request_reads = 0;
        end_offer();
        return;
    }
    const uintptr_t world = load((uintptr_t)game + WORLD_CHR_MAN);
    const uintptr_t player = pointer_ok(world) ? load(world + 0x60) : 0;
    if (!pointer_ok(player)) return; /* loading or in the title menus */
    if (get(entered_flag)) {
        int gate = 0;
        for (int k = 0; k < bit_count; ++k) gate |= get(bits_flag + k) << k;
        set(entered_flag, 0);
        entered = gate < gate_count ? gate : -1;
    }
    /* Walking off to another map forgets the gate (blocks of one map stream in together). */
    int32_t block;
    if (entered >= 0 && fight < 0 && player_block(&block) && ((block >> 24) & 0xff) != gates[entered].area)
        entered = -1;
    const int bar = major_bar(world);
    const int32_t life = hp(player);
    if (fight < 0 && bar >= 0 && entered >= 0 && !get(gates[entered].defeat_flag)) {
        fight = entered;
        fight_name = bar;
        bar_gone = 0;
        printf("Boss retry: fighting %s (gate %d)\n", boss_name(bar), fight);
    }
    if (fight >= 0) {
        if (life == 0) {
            pending = fight;
            pending_name = fight_name;
            fight = entered = -1;
            printf("Boss retry: died fighting %s\n", boss_name(pending_name));
        } else if (bar < 0 && ++bar_gone > BAR_GONE_READS) {
            fight = entered = -1; /* the boss died, or the fight ended another way */
        } else if (bar >= 0) {
            bar_gone = 0;
        }
    }
    if (pending < 0) return;
    /* Alive again (no reload to hold), the boss died too, or no lamp to arrive at. */
    if (life > 0 || get(gates[pending].defeat_flag) || !single_player() || !gates[pending].lamp) {
        end_offer();
        return;
    }
    if (!held_reload || start_menu_open() || bbgpu_overlay_captures_input()) return;
    bbgpu_retry_prompt_open(boss_name(pending_name), question, retry_text, decline_text);
    shown = 1;
}
