/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
void bbgpu_before_input(void);
/* Lamp travel menu (src/runtime_lamp.c, gpu/shim/bbport_lamp_menu.cpp). load reads the
 * generator's lamp_menu.txt and returns the number of lamps (0 on failure) with the event flags
 * the talk scripts use; open shows it (lit: one byte per lamp, here: the lamp the player stands
 * at or -1); result returns a lamp index, BBGPU_LAMP_DREAM, BBGPU_LAMP_CANCEL or, while the
 * player chooses, BBGPU_LAMP_PENDING. The menu holds the game's pad input neutral. */
#define BBGPU_LAMP_MENU_MAX 64
enum { BBGPU_LAMP_PENDING = -3, BBGPU_LAMP_DREAM = -2, BBGPU_LAMP_CANCEL = -1 };
int bbgpu_lamp_menu_load(const char *path, int *request, int *done, int *dream,
                         const int **lamp_flags, const int **lit_flags);
void bbgpu_lamp_menu_open(const unsigned char *lit, int here);
void bbgpu_lamp_menu_close(void);
int bbgpu_lamp_menu_result(void);
/* Boss retry prompt (src/runtime_retry.c, drawn with the lamp menu): open shows the boss's name,
 * the question and the two choices (UTF-8); result returns BBGPU_RETRY_PENDING while the player
 * chooses, then 1 (retry) or 0 (not now). It holds the game's pad input neutral too. */
enum { BBGPU_RETRY_PENDING = -1 };
void bbgpu_retry_prompt_open(const char *boss, const char *question, const char *retry, const char *decline);
void bbgpu_retry_prompt_close(void);
int bbgpu_retry_prompt_result(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
/* Completed and current cooperative pause time; the window event loop keeps running. */
uint64_t bbgpu_background_paused_ns(void);
#ifdef __cplusplus
}
#endif
#endif
