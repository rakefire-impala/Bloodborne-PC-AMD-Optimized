/* Discord Rich Presence through the Discord client's local IPC (the protocol the discord-rpc
 * library implements: a named pipe on Windows, a Unix socket elsewhere, frames of opcode,
 * length and JSON). No library or DLL: Discord not running just means no presence.
 *
 * The area comes from the map layouts the game loads (map/MapStudio/mAA_BB_CC_DD.msb): the
 * game's place-name IDs follow the same numbering (2410 Central Yharnam is m24_01), and the
 * developers' comments in map/worldmsblist.worldloadlistlist agree (m27 "forest", m36 "fishing
 * village"). A load that brings several blocks of one map shows the map's general name.
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

/* Map layouts opened by the game; a gap of LOAD_GAP_NS starts a new load. */
#define LOAD_GAP_NS 3000000000ull
#define MAX_BLOCKS 16
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int map, block; } loaded[MAX_BLOCKS];
static int loaded_count;
static uint64_t last_layout_ns;
static const Zone *zone; /* current area, NULL before the first map */
static int started;
static char app_id[32];

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

/* Called for every guest file the game opens: cheap unless it is a map layout. */
void runtime_discord_note_open(const char *path) {
    if (!started || !path) return;
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
static void presence_json(char *out, size_t size, const Zone *current, long long start, unsigned long nonce) {
    char details[64], state[96], image[64];
    if (current) {
        snprintf(details, sizeof(details), "Exploring");
        snprintf(state, sizeof(state), "%s", current->name);
        snprintf(image, sizeof(image), "%s", current->image);
    } else {
        snprintf(details, sizeof(details), "In the menus");
        state[0] = '\0';
        snprintf(image, sizeof(image), "bloodborne");
    }
    snprintf(out, size, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%lu\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"",
             nonce, process_id());
    append_json_string(out, size, details);
    if (state[0]) {
        strncat(out, "\",\"state\":\"", size - strlen(out) - 1);
        append_json_string(out, size, state);
    }
    /* The cover art large; the area's image in the small circle, its name on hover. */
    char rest[512];
    snprintf(rest, sizeof(rest), "\",\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"bloodborne\","
             "\"large_text\":\"Bloodborne\"", start);
    strncat(out, rest, size - strlen(out) - 1);
    if (current) {
        snprintf(rest, sizeof(rest), ",\"small_image\":\"%s\",\"small_text\":\"", image);
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
    int sent_any = 0;
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
        pthread_mutex_unlock(&state_lock);
        /* Discord accepts 5 updates per 20 s: one per change, at most every 4 s. */
        if ((sent_any && current == sent) || (sent_any && now - last_sent < 4000000000ull)) continue;
        char json[1024];
        presence_json(json, sizeof(json), current, start, ++nonce);
        if (!send_frame(OP_FRAME, json) || !drain(1000)) {
            ipc_close();
            connected = 0;
            next_attempt = now + 15ull * 1000000000ull;
            continue;
        }
        sent = current;
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
