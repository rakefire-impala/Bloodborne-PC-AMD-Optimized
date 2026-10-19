/* Discord Rich Presence through the Discord client's local IPC (the protocol the discord-rpc
 * library implements: a named pipe on Windows, a Unix socket elsewhere, frames of opcode,
 * length and JSON). No library or DLL: Discord not running just means no presence.
 *
 * The area is the map block the player is in (mAA_BB): the game's place-name IDs follow the
 * same numbering (2410 Central Yharnam is m24_01), and the developers' comments in
 * map/worldmsblist.worldloadlistlist agree (m27 "forest", m36 "fishing village"). On 1.09 it is
 * read from the game on its own thread (runtime_discord_poll), as the script binding
 * GetLastBlockId does. Map layouts opened are only a fallback for other executables: the game
 * also opens neighbouring maps it streams in (m27 and m24_00 from the Tomb of Oedon).
 *
 * Boss fights: the HUD's three boss bars (EMEVD 2003[11]) are slots in the menu manager holding
 * the boss's name ID (NPC names) and character handle. A bar counts while its character has HP
 * left, the HUD's own lookup; the presence then reads "Fighting <boss>" with the fight's time.
 *
 * BB_DISCORD=0 turns it off; BB_DISCORD_APP_ID=<id> uses another Discord application. Image
 * keys ("bloodborne" large, zones[] below small) are the asset names of that application. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define DEFAULT_APP_ID "1559429686302613586"

typedef struct { int map, block; const char *name, *image; } Zone;
/* block -1: the whole map (one entry per map, or its general name when several load). */
static const Zone zones[] = {
    {21, 0, "Hunter's Dream", "hunters_dream"},
    {21, 1, "Abandoned Old Workshop", "abandoned_old_workshop"},
    {21, -1, "Hunter's Dream", "hunters_dream"},
    {22, -1, "Hemwick Charnel Lane", "hemwick_charnel_lane"},
    {23, -1, "Old Yharnam", "old_yharnam"},
    {24, 0, "Cathedral Ward", "cathedral_ward"},
    {24, 1, "Central Yharnam", "central_yharnam"},
    {24, 2, "Upper Cathedral Ward", "upper_cathedral_ward"},
    {24, -1, "Yharnam", "yharnam"},
    {25, -1, "Forsaken Cainhurst Castle", "cainhurst_castle"},
    {26, -1, "Nightmare of Mensis", "nightmare_of_mensis"},
    {27, -1, "Forbidden Woods", "forbidden_woods"},
    {28, -1, "Yahar'gul, Unseen Village", "yahargul"},
    {29, -1, "Chalice Dungeon", "chalice_dungeon"},
    {32, -1, "Byrgenwerth", "byrgenwerth"},
    {33, -1, "Nightmare Frontier", "nightmare_frontier"},
    {34, -1, "Hunter's Nightmare", "hunters_nightmare"},
    {35, -1, "Research Hall", "research_hall"},
    {36, -1, "Fishing Hamlet", "fishing_hamlet"},
};

/* NPC names (item.msgbnd NPC名.fmg, English): the name IDs boss bars show. */
static const struct { int id; const char *name; } npc_names[] = {
    {1000, "Madman's Escort"}, {1010, "Forgotten Madman"}, {6400, "Old Hunter Henryk"},
    {6500, "Valtr, Master of the League"}, {6505, "Valtr, Beast Eater"},
    {6530, "Yamamura the Wanderer"}, {6540, "Younger Madaras Twin"},
    {6550, "Mensis Scholar Damian"}, {6600, "Henriett"}, {6640, "Defector Antal"},
    {7040, "Tomb Prospector Olek"}, {7045, "Tomb Prospector Gremia"}, {7060, "Beastclaw Jozef"},
    {7160, "Queen Killer"}, {7300, "Vitus"}, {7310, "Madman Wallar"},
    {101000, "Degenerate Beast"}, {106000, "Brainsucker"}, {127000, "Maneater Boar"},
    {209000, "Blood-starved Beast"}, {209010, "Blood-starved Beast"},
    {210000, "The Witch of Hemwick"}, {212010, "Shadow of Yharnam"},
    {212020, "Shadow of Yharnam"}, {212030, "Shadow of Yharnam"},
    {216000, "Keeper of the Old Lords"}, {218000, "Loran Silverbeast"},
    {232000, "Martyr Logarius"}, {251000, "Ebrietas, Daughter of the Cosmos"},
    {251010, "Ebrietas, Daughter of the Cosmos"}, {257000, "Celestial Emissary"},
    {257010, "Celestial Emissary"}, {271000, "Father Gascoigne"}, {272000, "Father Gascoigne"},
    {304000, "Merciless Watcher"}, {304001, "Watcher Chieftain"}, {304002, "Merciless Watcher"},
    {304003, "Merciless Watcher"}, {305000, "Pthumerian Descendant"},
    {305001, "Pthumerian Descendant"}, {305002, "Pthumerian Elder"},
    {306000, "Pthumerian Bride"}, {306001, "Yharnam, Pthumerian Queen"},
    {313000, "Undead Giant"}, {403000, "Living Failures"}, {450000, "Laurence, the First Vicar"},
    {451000, "Ludwig the Accursed"}, {451005, "Ludwig, the Holy Blade"},
    {452000, "Lady Maria of the Astral Clocktower"}, {453000, "Orphan of Kos"},
    {454000, "Orphan of Kos"}, {500000, "Cleric Beast"}, {501000, "Watchdog of the Old Lords"},
    {502000, "Vicar Amelia"}, {504000, "Loran Darkbeast"}, {504010, "Abhorrent Beast"},
    {507000, "The One Reborn"}, {508000, "Darkbeast Paarl"}, {508010, "Loran Darkbeast"},
    {509000, "Bloodletting Beast"}, {509010, "Bloodletting Beast"},
    {510000, "Rom, the Vacuous Spider"}, {510010, "Rom, the Vacuous Spider"},
    {511000, "Great One Beast"}, {512000, "Amygdala"}, {512010, "Amygdala"},
    {540000, "Moon Presence"}, {551000, "Mergo's Wet Nurse"}, {551010, "Mergo's Wet Nurse"},
    {750000, "Beast-possessed Soul"}, {802000, "Slug Princess"}, {803000, "Heroine Doll"},
    {804000, "Gehrman, the First Hunter"}, {805000, "Gehrman, the First Hunter"},
    {899000, "Micolash, Host of the Nightmare"}, {899010, "Vileblood Hunter Alfred"},
    {900020, "The Vileblood"}, {900030, "The Executioners"}, {900040, "Hunter of Hunters"},
    {901040, "Nameless Tomb Prospector"}, {901060, "Izzy's Admirer"},
    {901070, "Nameless Madman"}, {901110, "Bone Ash Hunter Carla"},
    {901150, "Drifting Vileblood Leo"}, {901160, "Ardeo Man"}, {901170, "Defeated Hunter"},
    {901180, "Choir Intelligencer Edgar"},
};

/* Map layouts opened by the game; a gap of LOAD_GAP_NS starts a new load. */
#define LOAD_GAP_NS 3000000000ull
#define MAX_BLOCKS 16
/* Out of the world this long (not a loading screen): the title screen or its menus. */
#define MENUS_AFTER_NS 45000000000ull
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int map, block; } loaded[MAX_BLOCKS];
static int loaded_count;
static uint64_t last_layout_ns;
static const Zone *zone; /* current area, NULL before the first map */
static int boss = -1;    /* name ID of the boss bar being fought, -1 when none */
static long long fight_start;
static int started;
static char app_id[32];

/* The 1.09 game state the poll reads; game is NULL when the executable check failed. */
enum {
    WORLD_CHR_MAN = 0x553e878, /* WorldChrMan *: +0x60 local player, +0x850 character sets */
    BLOCK_LIST = 0x553b148,    /* +0x20 index of the player's block, +0x10 -> +0x18 count, +0x20 entries */
    MENU_MAN = 0x5562878,      /* menu manager *: boss bars at +0x1658, 3 x {name ID, handle, ...} */
    BOSS_BARS = 0x1658,
};
static unsigned char *game;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
#endif
}
static const Zone *find_zone(int map, int block) {
    const Zone *whole = NULL;
    for (size_t i = 0; i < sizeof(zones) / sizeof(*zones); ++i) {
        if (zones[i].map != map) continue;
        if (zones[i].block == block) return &zones[i];
        if (zones[i].block < 0) whole = &zones[i];
    }
    return whole;
}

/* Called for every guest file the game opens: cheap unless it is a map layout. Only used when
   the poll cannot read the player's block. */
void runtime_discord_note_open(const char *path) {
    if (!started || game || !path) return;
    const char *msb = strstr(path, ".msb");
    if (!msb || msb - path < 12) return;
    const char *m = msb - 12; /* mAA_BB_CC_DD */
    if (tolower((unsigned char)m[0]) != 'm' || m[3] != '_' || m[6] != '_' || m[9] != '_' ||
        (m != path && m[-1] != '/' && m[-1] != '\\'))
        return;
    if (!isdigit((unsigned char)m[1]) || !isdigit((unsigned char)m[2]) ||
        !isdigit((unsigned char)m[4]) || !isdigit((unsigned char)m[5]))
        return;
    const int map = (m[1] - '0') * 10 + (m[2] - '0'), block = (m[4] - '0') * 10 + (m[5] - '0');
    pthread_mutex_lock(&state_lock);
    const uint64_t now = now_ns();
    if (now - last_layout_ns > LOAD_GAP_NS) loaded_count = 0;
    last_layout_ns = now;
    int known = 0;
    for (int i = 0; i < loaded_count; ++i) known |= loaded[i].map == map && loaded[i].block == block;
    if (!known && loaded_count < MAX_BLOCKS) {
        loaded[loaded_count].map = map;
        loaded[loaded_count].block = block;
        ++loaded_count;
    }
    /* One block: that block's name. Several of one map: the map's name. Several maps: the
       first one's (where the load began). */
    int same_map = 1, same_block = 1;
    for (int i = 1; i < loaded_count; ++i) {
        same_map &= loaded[i].map == loaded[0].map;
        same_block &= same_map && loaded[i].block == loaded[0].block;
    }
    const Zone *next = find_zone(loaded[0].map, same_block ? loaded[0].block : -1);
    char list[MAX_BLOCKS * 7 + 1] = "";
    for (int i = 0; i < loaded_count; ++i) {
        char item[8];
        snprintf(item, sizeof(item), " m%02d_%02d", loaded[i].map % 100, loaded[i].block % 100);
        strcat(list, item);
    }
    const int changed = next && next != zone;
    if (next) zone = next;
    pthread_mutex_unlock(&state_lock);
    if (changed) printf("Discord: area %s (map layouts:%s)\n", next->name, list);
}

/* ---- Game state (1.09) --------------------------------------------------------------------- */
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

void runtime_discord_install(void *image, size_t size, const char *app_version) {
    static const struct { size_t offset; unsigned char length; unsigned char bytes[64]; } checks[] = {
        /* GetLastBlockId: lea rcx,[block list]; ... movsxd rcx,[rdx+0x20]; mov rdx,[rdx+0x10];
         * cmp [rdx+0x18],ecx; mov rdx,[rdx+0x20]; entry rcx*0xa0; mov ecx,[rdx+rcx+8]. */
        {0x1332db8, 47, {0x48,0x8d,0x0d,0x89,0x83,0x20,0x04,0x48,0x8b,0x11,0x48,0x85,0xd2,0x74,0x31,
                         0x48,0x63,0x4a,0x20,0x48,0x85,0xc9,0x78,0x28,0x48,0x8b,0x52,0x10,0x39,0x4a,
                         0x18,0x7e,0x1f,0x48,0x8b,0x52,0x20,0x48,0x8d,0x0c,0x89,0x48,0xc1,0xe1,0x05,
                         0x8b,0x4c}},
        /* The boss bar setter (EMEVD 2003[11]): slot <= 2, [menu+4*5*slot+0x165c] = handle,
         * [+0x1658] = name ID. */
        {0x131ef3f, 52, {0x83,0xfb,0x02,0x77,0x2f,0x4c,0x8b,0x3d,0x2d,0x39,0x24,0x04,0x31,0xd2,0x89,
                         0xf7,0xe8,0x4c,0xa8,0x0a,0x00,0x48,0x85,0xc0,0x74,0x1a,0x48,0x63,0xcb,0x8b,
                         0x40,0x08,0x48,0x8d,0x0c,0x89,0x41,0x89,0x84,0x8f,0x5c,0x16,0x00,0x00,0x45,
                         0x89,0xb4,0x8f,0x58,0x16,0x00,0x00}},
        /* The boss bar's HUD: handle bits 14-19 select WorldChrMan +0x850 + 8 * set, bits 0-13
         * an entry of 0x38; ChrIns +0x3b0 -> +0x20 -> HP at +0xf8. */
        {0x164c3e2, 62, {0xc4,0xe2,0x68,0xf7,0xd0,0x48,0x8b,0x71,0x08,0x8b,0x76,0x18,0x83,0xc6,0x02,
                         0x39,0xf2,0x0f,0x8d,0xf3,0x02,0x00,0x00,0x89,0xd2,0x48,0x8b,0x8c,0xd1,0x50,
                         0x08,0x00,0x00,0x48,0x85,0xc9,0x0f,0x84,0xe0,0x02,0x00,0x00,0x25,0xff,0x3f,
                         0x00,0x00,0x3b,0x01,0x0f,0x8d,0xd3,0x02,0x00,0x00,0x48,0x6b,0xc0,0x38,0x48,
                         0x03,0x41}},
        {0x164c427, 31, {0x48,0x8b,0x00,0x48,0x85,0xc0,0x0f,0x84,0xb9,0x02,0x00,0x00,0x48,0x8b,0x80,
                         0xb0,0x03,0x00,0x00,0x48,0x8b,0x40,0x20,0xc5,0xfa,0x2a,0x80,0xf8,0x00,0x00,
                         0x00}},
    };
    const unsigned char *code = image;
    int ok = app_version && !strcmp(app_version, "01.09") && size > MENU_MAN + 8;
    for (size_t i = 0; ok && i < sizeof(checks) / sizeof(checks[0]); ++i)
        ok = memcmp(code + checks[i].offset, checks[i].bytes, checks[i].length) == 0;
    if (!ok) {
        puts("Discord: executable/version guard failed; areas from map loads, no boss fights");
        return;
    }
    game = image;
}

/* The block the player is in, as GetLastBlockId reads it: 0xAABBCCDD for mAA_BB_CC_DD. */
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

/* The first boss bar whose character still has HP; its name ID, or -1. */
static int fought_boss(uintptr_t world) {
    const uintptr_t menu = load((uintptr_t)game + MENU_MAN);
    const uintptr_t info = load(world + 8);
    if (!pointer_ok(menu) || !pointer_ok(info)) return -1;
    const int32_t sets = load32(info + 0x18) + 2;
    for (int slot = 0; slot < 3; ++slot) {
        const int32_t name = load32(menu + BOSS_BARS + 20 * slot);
        const uint32_t handle = (uint32_t)load32(menu + BOSS_BARS + 20 * slot + 4);
        if (name < 0 || handle == 0xffffffffu) continue;
        const int32_t set = (int32_t)((handle >> 14) & 0x3f), entry = (int32_t)(handle & 0x3fff);
        if (set >= sets) continue;
        const uintptr_t characters = load(world + 0x850 + 8 * (uintptr_t)set);
        if (!pointer_ok(characters) || entry >= load32(characters)) continue;
        const uintptr_t entries = load(characters + 8);
        if (!pointer_ok(entries)) continue;
        const uintptr_t chr = load(entries + (uintptr_t)entry * 0x38);
        const uintptr_t modules = pointer_ok(chr) ? load(chr + 0x3b0) : 0;
        const uintptr_t stats = pointer_ok(modules) ? load(modules + 0x20) : 0;
        if (pointer_ok(stats) && load32(stats + 0xf8) > 0) return name;
    }
    return -1;
}

static const char *npc_name(int id) {
    for (size_t i = 0; i < sizeof(npc_names) / sizeof(*npc_names); ++i)
        if (npc_names[i].id == id) return npc_names[i].name;
    return NULL;
}

/* Called on the game's own thread once per pad read (as the lamp menu and safe menu are). */
void runtime_discord_poll(void) {
    static unsigned reads;
    static uint64_t left_world;
    if (!started || !game || ++reads % 15) return;
    const uintptr_t world = load((uintptr_t)game + WORLD_CHR_MAN);
    const int in_world = pointer_ok(world) && pointer_ok(load(world + 0x60));
    int32_t block = -1;
    const Zone *here = NULL;
    if (in_world && player_block(&block)) here = find_zone((block >> 24) & 0xff, (block >> 16) & 0xff);
    const int fought = in_world ? fought_boss(world) : -1;
    const uint64_t now = now_ns();
    if (in_world) left_world = 0;
    else if (!left_world) left_world = now;
    pthread_mutex_lock(&state_lock);
    const Zone *before = zone;
    const int boss_before = boss;
    if (here) zone = here;
    else if (!in_world && now - left_world > MENUS_AFTER_NS) zone = NULL;
    /* A second phase (another bar name) is the same fight. */
    if (fought >= 0 && boss < 0) fight_start = (long long)time(NULL);
    boss = fought;
    const Zone *after = zone;
    pthread_mutex_unlock(&state_lock);
    if (after != before) {
        if (after) printf("Discord: area %s (m%02d_%02d)\n", after->name, (block >> 24) & 0xff, (block >> 16) & 0xff);
        else puts("Discord: in the menus");
    }
    if (fought != boss_before) {
        const char *name = fought >= 0 ? npc_name(fought) : NULL;
        if (fought >= 0) printf("Discord: fighting %s (name %d)\n", name ? name : "a boss", fought);
        else puts("Discord: boss fight over");
    }
}

/* ---- IPC ----------------------------------------------------------------------------------- */
enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };
#ifdef _WIN32
static HANDLE pipe_handle = INVALID_HANDLE_VALUE;
static int ipc_open(void) {
    for (int i = 0; i < 10; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { pipe_handle = h; return 1; }
    }
    return 0;
}
static void ipc_close(void) {
    if (pipe_handle != INVALID_HANDLE_VALUE) CloseHandle(pipe_handle);
    pipe_handle = INVALID_HANDLE_VALUE;
}
static int ipc_write(const void *data, size_t size) {
    DWORD written = 0;
    return WriteFile(pipe_handle, data, (DWORD)size, &written, NULL) && written == size;
}
/* Bytes ready to read without blocking; -1 when the pipe broke. */
static long ipc_available(void) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe_handle, NULL, 0, NULL, &available, NULL)) return -1;
    return (long)available;
}
static int ipc_read(void *data, size_t size) {
    for (size_t done = 0; done < size;) {
        DWORD got = 0;
        if (!ReadFile(pipe_handle, (char *)data + done, (DWORD)(size - done), &got, NULL) || !got) return 0;
        done += got;
    }
    return 1;
}
static unsigned long process_id(void) { return (unsigned long)GetCurrentProcessId(); }
#else
static int ipc_socket = -1;
static int ipc_open(void) {
    const char *roots[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
    const char *subdirs[] = {"", "/app/com.discordapp.Discord", "/snap.discord"};
    for (size_t r = 0; r < sizeof(roots) / sizeof(*roots); ++r) {
        if (!roots[r]) continue;
        for (size_t s = 0; s < sizeof(subdirs) / sizeof(*subdirs); ++s) {
            for (int i = 0; i < 10; ++i) {
                struct sockaddr_un address = {.sun_family = AF_UNIX};
                snprintf(address.sun_path, sizeof(address.sun_path), "%s%s/discord-ipc-%d", roots[r], subdirs[s], i);
                const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) return 0;
                if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) { ipc_socket = fd; return 1; }
                close(fd);
            }
        }
    }
    return 0;
}
static void ipc_close(void) {
    if (ipc_socket >= 0) close(ipc_socket);
    ipc_socket = -1;
}
static int ipc_write(const void *data, size_t size) {
    for (size_t done = 0; done < size;) {
        const ssize_t n = send(ipc_socket, (const char *)data + done, size - done, MSG_NOSIGNAL);
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}
static long ipc_available(void) {
    struct pollfd p = {.fd = ipc_socket, .events = POLLIN};
    const int r = poll(&p, 1, 0);
    if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
    return r > 0 ? 1 : 0;
}
static int ipc_read(void *data, size_t size) {
    for (size_t done = 0; done < size;) {
        const ssize_t n = recv(ipc_socket, (char *)data + done, size - done, 0);
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}
static unsigned long process_id(void) { return (unsigned long)getpid(); }
#endif

static int send_frame(unsigned opcode, const char *json) {
    const size_t length = strlen(json);
    unsigned char header[8];
    for (int i = 0; i < 4; ++i) {
        header[i] = (unsigned char)(opcode >> (8 * i));
        header[4 + i] = (unsigned char)(length >> (8 * i));
    }
    return ipc_write(header, sizeof(header)) && ipc_write(json, length);
}
/* Reads one frame into `json` (NUL-terminated, cut to its size); returns the opcode or -1. */
static int read_frame(char *json, size_t size) {
    unsigned char header[8];
    if (!ipc_read(header, sizeof(header))) return -1;
    const unsigned opcode = header[0] | header[1] << 8 | header[2] << 16 | (unsigned)header[3] << 24;
    const size_t length = header[4] | header[5] << 8 | header[6] << 16 | (size_t)header[7] << 24;
    if (length > 1 << 20) return -1;
    char discard[256];
    for (size_t done = 0; done < length;) {
        const size_t n = length - done < sizeof(discard) ? length - done : sizeof(discard);
        if (!ipc_read(discard, n)) return -1;
        if (done < size - 1) memcpy(json + done, discard, done + n < size - 1 ? n : size - 1 - done);
        done += n;
    }
    json[length < size - 1 ? length : size - 1] = '\0';
    return (int)opcode;
}
/* Handles what Discord sent meanwhile (replies, pings); 0 when the connection is gone. */
static int drain(unsigned wait_ms) {
    for (unsigned waited = 0;;) {
        const long available = ipc_available();
        if (available < 0) return 0;
        if (available == 0) {
            if (waited >= wait_ms) return 1;
            sleep_ms(20);
            waited += 20;
            continue;
        }
        char json[2048];
        const int opcode = read_frame(json, sizeof(json));
        if (opcode < 0 || opcode == OP_CLOSE) {
            if (opcode == OP_CLOSE) printf("Discord: connection closed (%s)\n", json);
            return 0;
        }
        if (opcode == OP_PING && !send_frame(OP_PONG, json)) return 0;
        if (opcode == OP_FRAME && strstr(json, "\"evt\":\"ERROR\"")) {
            static int reported;
            if (reported++ < 4) printf("Discord: %s\n", json);
        } else if (opcode == OP_FRAME && strstr(json, "\"cmd\":\"SET_ACTIVITY\"")) {
            static int accepted;
            if (!accepted++) puts("Discord: presence accepted");
        }
    }
}
static int connect_discord(void) {
    if (!ipc_open()) return 0;
    char json[128];
    snprintf(json, sizeof(json), "{\"v\":1,\"client_id\":\"%s\"}", app_id);
    char reply[2048] = "";
    if (send_frame(OP_HANDSHAKE, json)) {
        for (int waited = 0; waited < 3000 && ipc_available() == 0; waited += 20) sleep_ms(20);
        if (ipc_available() > 0 && read_frame(reply, sizeof(reply)) == OP_FRAME && strstr(reply, "READY")) {
            printf("Discord: connected (application %s)\n", app_id);
            return 1;
        }
    }
    printf("Discord: handshake refused (%s)\n", reply[0] ? reply : "no reply");
    ipc_close();
    return -1;
}

static void append_json_string(char *out, size_t size, const char *text) {
    size_t at = strlen(out);
    for (const char *c = text; *c && at + 3 < size; ++c) {
        if (*c == '"' || *c == '\\') out[at++] = '\\';
        out[at++] = *c;
    }
    out[at] = '\0';
}
static void presence_json(char *out, size_t size, const Zone *current, int fought, long long start,
                          unsigned long nonce) {
    char details[96];
    if (fought >= 0) {
        const char *name = npc_name(fought);
        snprintf(details, sizeof(details), "Fighting %s", name ? name : "a boss");
    } else {
        snprintf(details, sizeof(details), "%s", current ? "Exploring" : "In the menus");
    }
    snprintf(out, size, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%lu\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"",
             nonce, process_id());
    append_json_string(out, size, details);
    if (current) {
        strncat(out, "\",\"state\":\"", size - strlen(out) - 1);
        append_json_string(out, size, current->name);
    }
    /* The cover art large; the area's image in the small circle, its name on hover. */
    char rest[512];
    snprintf(rest, sizeof(rest), "\",\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"bloodborne\","
             "\"large_text\":\"Bloodborne\"", start);
    strncat(out, rest, size - strlen(out) - 1);
    if (current) {
        snprintf(rest, sizeof(rest), ",\"small_image\":\"%s\",\"small_text\":\"", current->image);
        strncat(out, rest, size - strlen(out) - 1);
        append_json_string(out, size, current->name);
        strncat(out, "\"", size - strlen(out) - 1);
    }
    strncat(out, "}}}}", size - strlen(out) - 1);
}

static void *presence_thread(void *unused) {
    (void)unused;
    const long long start = (long long)time(NULL);
    unsigned long nonce = 0;
    int connected = 0, refused = 0, reported_absent = 0;
    const Zone *sent = NULL;
    int sent_boss = -1, sent_any = 0;
    uint64_t next_attempt = 0, last_sent = 0;
    for (;; sleep_ms(2000)) {
        const uint64_t now = now_ns();
        if (!connected) {
            if (now < next_attempt) continue;
            const int result = connect_discord();
            if (result <= 0) {
                if (!result && !reported_absent++) puts("Discord: not running; presence once it starts");
                /* A refused handshake (wrong application ID) is not retried as often. */
                next_attempt = now + (result < 0 || refused ? 120ull : 15ull) * 1000000000ull;
                refused |= result < 0;
                continue;
            }
            connected = 1;
            sent_any = 0;
        }
        if (!drain(0)) {
            puts("Discord: disconnected; reconnecting when it is back");
            ipc_close();
            connected = 0;
            next_attempt = now + 15ull * 1000000000ull;
            continue;
        }
        pthread_mutex_lock(&state_lock);
        const Zone *current = zone;
        const int fought = boss;
        const long long fought_since = fight_start;
        pthread_mutex_unlock(&state_lock);
        /* Discord accepts 5 updates per 20 s: one per change, at most every 4 s. */
        if ((sent_any && current == sent && fought == sent_boss) ||
            (sent_any && now - last_sent < 4000000000ull))
            continue;
        char json[1024];
        /* A fight shows its own time; exploring shows the session's. */
        presence_json(json, sizeof(json), current, fought, fought >= 0 ? fought_since : start, ++nonce);
        if (!send_frame(OP_FRAME, json) || !drain(1000)) {
            ipc_close();
            connected = 0;
            next_attempt = now + 15ull * 1000000000ull;
            continue;
        }
        sent = current;
        sent_boss = fought;
        sent_any = 1;
        last_sent = now;
    }
    return NULL;
}

void runtime_discord_start(void) {
    const char *mode = getenv("BB_DISCORD");
    if (mode && mode[0] == '0') return;
    const char *id = getenv("BB_DISCORD_APP_ID");
    snprintf(app_id, sizeof(app_id), "%s", id && id[0] ? id : DEFAULT_APP_ID);
    for (const char *c = app_id; *c; ++c) {
        if (!isdigit((unsigned char)*c)) { printf("Discord: application ID %s is not a number; off\n", app_id); return; }
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, presence_thread, NULL)) return;
    pthread_detach(thread);
    started = 1;
}
