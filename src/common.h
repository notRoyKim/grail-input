/*
 * grail-input - keyboard / gamepad input mod for Grail (Sokpop)
 *
 * version.dll proxy that the game loads from its own folder.
 *   - gamepad (XInput) and keyboard drive a virtual mouse cursor
 *   - D-pad / arrow keys jump between the game's own hover targets
 *     (found by hooking the game's mouse_on / mouse_on_between scripts)
 *   - optional Parsec cursor fix (formerly parsec_cursor_fix)
 */
#ifndef GRAIL_INPUT_COMMON_H
#define GRAIL_INPUT_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#define GI_VERSION "1.0.0-beta.1"

/* ---------- logging (main.c) ---------- */
extern int g_log_level; /* 0 off, 1 normal, 2 verbose */
void gi_log(int level, const char *fmt, ...);
#define LOG1(...) gi_log(1, __VA_ARGS__)
#define LOG2(...) gi_log(2, __VA_ARGS__)

/* ---------- time ---------- */
double gi_now_ms(void);

/* ---------- config (main.c) ---------- */
enum {
    ACT_CLICK, ACT_RCLICK, ACT_ESC, ACT_CENTER,
    ACT_WHEEL_UP, ACT_WHEEL_DOWN,
    ACT_NAV_UP, ACT_NAV_DOWN, ACT_NAV_LEFT, ACT_NAV_RIGHT,
    ACT_SLOW, ACT_FAST,
    ACT_COUNT
};
extern const char *const g_act_names[ACT_COUNT];

/* XInput-style button bits, plus triggers as extra bits */
#define PAD_LT 0x10000u
#define PAD_RT 0x20000u

enum { STICK_NONE = 0, STICK_LEFT = 1, STICK_RIGHT = 2 };
enum { CURSORFIX_OFF = 0, CURSORFIX_TRANSPARENT = 1, CURSORFIX_ARROW = 2 };
enum { OVERRIDE_NEVER = 0, OVERRIDE_AUTO = 1, OVERRIDE_ALWAYS = 2 };

#define MAX_KEYS_PER_ACT 4

typedef struct {
    int enabled;

    /* gamepad */
    int pad_enabled;
    uint32_t pad_map[ACT_COUNT];
    int stick_cursor, stick_scroll;
    float deadzone;
    float cursor_speed;   /* screen heights per second at full tilt */
    float slow_mul, fast_mul;
    float scroll_rate;    /* wheel notches per second at full tilt */

    /* keyboard */
    int kb_enabled;
    int key_map[ACT_COUNT][MAX_KEYS_PER_ACT];
    int free_move_mod;    /* VK; when held, arrow nav keys move the cursor freely */
    float kb_cursor_speed;

    /* smart navigation */
    int nav_smart;
    int repeat_delay_ms, repeat_interval_ms;
    float nav_step;       /* fallback step (fraction of client height) */
    float min_size_px;
    float max_area;       /* fraction of client area */
    float perp_weight;

    /* cursor */
    int cursorfix_mode;
    int override_mode;
    int background_input; /* act even when the game window is not focused */
} GiConfig;

extern GiConfig g_cfg;
void config_load(const wchar_t *ini_path);

/* ---------- game hooks (gml_hooks.c) ---------- */
typedef struct { float cx, cy, w, h; } NavRect;

int  hooks_install(void);            /* returns bitmask of installed hooks */
void hooks_install_iat(void);        /* SetCursor + GetCursorPos */
int  hooks_snapshot(NavRect *out, int max, double *age_ms);
int  hooks_mouse_sample(double *gx, double *gy, uint32_t *frame);
uint32_t hooks_frame(void);          /* frame counter (Step hook), 0 if unavailable */
int  hooks_have_frames(void);
int  hooks_have_rects(void);

/* cursor override shared with input.c */
extern volatile LONG g_override_active;
extern volatile LONG64 g_virt_screen;   /* packed: x in low 32, y in high 32 */
BOOL real_GetCursorPos(POINT *p);
extern volatile LONG g_key_latch[256];

/* ---------- input thread (input.c) ---------- */
void input_start(void);

#endif
