/* SPDX-License-Identifier: GPL-2.0-or-later
 * Lamp travel menu (launcher: Quality of Life). The generated lamp talk scripts
 * (scripts/lamp_travel.py) ask for a choice through event flags; this reads and sets them
 * with the game's own event flag functions and hands the menu itself to the overlay
 * (gpu/shim/bbport_lamp_menu.cpp). Runs on the thread that reads the pad, once per read.
 *
 * Protocol (flag numbers come from BB_LAMP_MENU, the generator's lamp_menu.txt): the talk
 * script clears DONE, DREAM and every lamp flag, sets its own lamp's flag and REQUEST. Seeing
 * REQUEST without DONE, this clears that lamp flag and opens the menu with the lit lamps;
 * the result sets a lamp's flag or DREAM (nothing when cancelled) and then DONE. The talk
 * script clears REQUEST and DONE, and the flag it acts on. */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    IS_EVENT_FLAG = 0x1399bf0,   /* bool (ignored, flag id): the "IsEventFlag" binding's target */
    SET_EVENT_FLAG = 0x13cfcc0,  /* (SprjEventFlagMan *, flag id, value) */
    SET_BINDING = 0x1258310,     /* "SetEventFlag": checks both globals below, then calls it */
    FLAGS_READY = 0x553b0d8,     /* non-null once the game has event flags */
    EVENT_FLAG_MAN = 0x553b100,  /* SprjEventFlagMan singleton */
};
typedef int (ABI *IsEventFlag)(void *, uint32_t);
typedef void (ABI *SetEventFlag)(void *, uint32_t, uint32_t);

static unsigned char *game;
static int active, shown;
static int request, done, dream, count;
static const int *lamp_flags, *lit_flags;

static int ready(void) {
    void *man;
    memcpy(&man, game + EVENT_FLAG_MAN, sizeof(man));
    void *flags;
    memcpy(&flags, game + FLAGS_READY, sizeof(flags));
    return man && flags;
}
static int get(int flag) {
    return ((IsEventFlag)(game + IS_EVENT_FLAG))(NULL, (uint32_t)flag) & 1;
}
static void set(int flag, int value) {
    void *man;
    memcpy(&man, game + EVENT_FLAG_MAN, sizeof(man));
    ((SetEventFlag)(game + SET_EVENT_FLAG))(man, (uint32_t)flag, (uint32_t)value);
}

void runtime_lamp_install(void *image, size_t size, const char *app_version) {
    static const unsigned char is_flag[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,
                                            0x41,0x89,0xf7,0x4c,0x8d,0x35,0xfc,0x14,0x1a,0x04};
    static const unsigned char set_flag[] = {0x41,0x89,0xd0,0x8b,0x4f,0x1c,0x31,0xd2,0x89,0xf0,
                                             0xf7,0xf1,0x41,0x89,0xc1,0x44,0x0f,0xaf,0xc9};
    static const unsigned char binding[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,
                                            0x41,0x89,0xd6,0x89,0xf3,0x48,0x8d,0x05,0xb2,0x2d,0x2e,0x04,
                                            0x48,0x83,0x38,0x00,0x74,0x4c,0x4c,0x8d,0x3d,0xcd,0x2d,0x2e,0x04};
    const char *menu = getenv("BB_LAMP_MENU");
    if (!menu || !*menu) return;
    const unsigned char *code = image;
    if (!app_version || strcmp(app_version, "01.09") || size < EVENT_FLAG_MAN + 8 ||
        memcmp(code + IS_EVENT_FLAG, is_flag, sizeof(is_flag)) ||
        memcmp(code + SET_EVENT_FLAG, set_flag, sizeof(set_flag)) ||
        memcmp(code + SET_BINDING, binding, sizeof(binding))) {
        puts("Lamp travel: executable/version guard failed; the travel menu is off");
        return;
    }
    count = bbgpu_lamp_menu_load(menu, &request, &done, &dream, &lamp_flags, &lit_flags);
    if (count <= 0) {
        printf("Lamp travel: cannot read %s; the travel menu is off\n", menu);
        return;
    }
    game = image;
    active = 1;
    printf("Lamp travel: menu of %d lamps (flags %d-%d)\n", count, request, lamp_flags[count - 1]);
}

/* bbport: logs every change of a lamp's lit flag with the time (a lamp lit in Cathedral Ward was
 * reported unlit again after a death; this shows when, and what the save loaded had). */
static void watch_lit(void) {
    static unsigned char last[BBGPU_LAMP_MENU_MAX];
    static int known, polls;
    if (++polls % 30) return;
    for (int i = 0; i < count; ++i) {
        const unsigned char lit = (unsigned char)get(lit_flags[i]);
        if (known && lit == last[i]) continue;
        if (known || lit) {
            const time_t now = time(NULL);
            char clock[16];
            strftime(clock, sizeof(clock), "%H:%M:%S", localtime(&now));
            printf("Lamp %s: flag %d at %s\n", lit ? "lit" : "UNLIT", lit_flags[i], clock);
        }
        last[i] = lit;
    }
    known = 1;
}

void runtime_lamp_poll(void) {
    if (!active || !ready()) return;
    watch_lit();
    if (!shown) {
        if (!get(request) || get(done)) return;
        unsigned char lit[BBGPU_LAMP_MENU_MAX];
        int here = -1;
        for (int i = 0; i < count; ++i) {
            lit[i] = (unsigned char)get(lit_flags[i]);
            if (here < 0 && get(lamp_flags[i])) here = i;
        }
        if (here >= 0) set(lamp_flags[here], 0);
        bbgpu_lamp_menu_open(lit, here);
        shown = 1;
        return;
    }
    /* A save loaded or the game reset meanwhile: nobody waits for the answer. */
    if (!get(request)) {
        bbgpu_lamp_menu_close();
        shown = 0;
        return;
    }
    const int result = bbgpu_lamp_menu_result();
    if (result == BBGPU_LAMP_PENDING) return;
    if (result >= 0 && result < count) set(lamp_flags[result], 1);
    else if (result == BBGPU_LAMP_DREAM) set(dream, 1);
    set(done, 1);
    shown = 0;
}
