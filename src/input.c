/*
 * input.c - input thread: gamepad (XInput) + keyboard -> virtual mouse
 */
#include "common.h"
#include <xinput.h>
#include <math.h>

/* ---------- window ---------- */
static HWND g_hwnd;
static DWORD g_pid;

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
    HWND *best = (HWND *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"YYGameMakerYY") == 0) { *best = h; return FALSE; }
    if (!*best) *best = h;
    return TRUE;
}

static HWND find_game_window(void)
{
    HWND best = NULL;
    EnumWindows(enum_cb, (LPARAM)&best);
    return best;
}

/* ---------- XInput ---------- */
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
static XInputGetState_t p_XInputGetState;
static double g_slot_retry[4];
static int g_slot_connected[4];

static void load_xinput(void)
{
    static const wchar_t *const dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !p_XInputGetState; i++) {
        HMODULE m = LoadLibraryW(dlls[i]);
        if (m) {
            p_XInputGetState = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
            if (p_XInputGetState) LOG1("xinput: using %ls", dlls[i]);
        }
    }
    if (!p_XInputGetState) LOG1("xinput: not available, gamepad disabled");
}

typedef struct {
    uint32_t buttons;           /* XInput bits + PAD_LT/PAD_RT */
    float lx, ly, rx, ry;       /* -1..1, y up positive */
    int connected;
} Pad;

static float axis_norm(SHORT v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

static void read_pad(Pad *out, double now)
{
    memset(out, 0, sizeof *out);
    if (!p_XInputGetState) return;
    for (DWORD i = 0; i < 4; i++) {
        /* polling an empty slot is slow, so only retry those now and then */
        if (!g_slot_connected[i] && now < g_slot_retry[i]) continue;
        XINPUT_STATE st;
        if (p_XInputGetState(i, &st) != ERROR_SUCCESS) {
            if (g_slot_connected[i]) LOG1("gamepad %lu disconnected", (unsigned long)i);
            g_slot_connected[i] = 0;
            g_slot_retry[i] = now + 1500.0;
            continue;
        }
        if (!g_slot_connected[i]) LOG1("gamepad %lu connected", (unsigned long)i);
        g_slot_connected[i] = 1;
        out->connected = 1;
        out->buttons |= st.Gamepad.wButtons;
        if (st.Gamepad.bLeftTrigger > 60) out->buttons |= PAD_LT;
        if (st.Gamepad.bRightTrigger > 60) out->buttons |= PAD_RT;
        float lx = axis_norm(st.Gamepad.sThumbLX), ly = axis_norm(st.Gamepad.sThumbLY);
        float rx = axis_norm(st.Gamepad.sThumbRX), ry = axis_norm(st.Gamepad.sThumbRY);
        if (lx * lx + ly * ly > out->lx * out->lx + out->ly * out->ly) { out->lx = lx; out->ly = ly; }
        if (rx * rx + ry * ry > out->rx * out->rx + out->ry * out->ry) { out->rx = rx; out->ry = ry; }
    }
}

/* ---------- output ---------- */
static void send_mouse(DWORD flags, DWORD data)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof in);
}

static void send_key(WORD vk, int up)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

/* ---------- virtual cursor ---------- */
static float g_vx, g_vy;          /* client coordinates */
static int g_cw, g_ch;            /* client size */
static int g_pad_mode;            /* we are driving the cursor */
static POINT g_commanded, g_anchor, g_last_real;

/* Recent positions we warped to.  Under Wine/Proton a warp comes back later as
 * a mouse-move event, so the "real" cursor can briefly show an older position
 * of ours; that must not count as the user grabbing the real mouse. */
#define ECHO_N 256
static POINT g_echo[ECHO_N];
static double g_echo_t[ECHO_N];
static int g_echo_i;

static void remember_commanded(POINT c)
{
    g_echo[g_echo_i] = c;
    g_echo_t[g_echo_i] = gi_now_ms();
    g_echo_i = (g_echo_i + 1) % ECHO_N;
}

static int is_echo(POINT p, double now)
{
    for (int i = 0; i < ECHO_N; i++) {
        if (now - g_echo_t[i] > 1000.0) continue;
        long dx = p.x - g_echo[i].x, dy = p.y - g_echo[i].y;
        if (dx * dx + dy * dy <= 4) return 1;
    }
    return 0;
}

static LONG64 pack_pt(POINT p) { return ((LONG64)(uint32_t)p.y << 32) | (uint32_t)p.x; }

static void set_override(int on)
{
    if (g_cfg.override_mode == OVERRIDE_ALWAYS) on = 1;
    if (g_cfg.override_mode == OVERRIDE_NEVER) on = 0;
    g_override_active = on;
}

static void move_cursor(float x, float y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_cw - 1) x = (float)(g_cw - 1);
    if (y > g_ch - 1) y = (float)(g_ch - 1);
    g_vx = x; g_vy = y;
    POINT c = { (LONG)lroundf(x), (LONG)lroundf(y) };
    ClientToScreen(g_hwnd, &c);
    if (!g_pad_mode) {
        g_pad_mode = 1;
        real_GetCursorPos(&g_anchor);
        LOG2("cursor: pad mode on");
    }
    g_commanded = c;
    remember_commanded(c);
    g_virt_screen = pack_pt(c);
    set_override(1);
    SetCursorPos(c.x, c.y);
}

static long dist2(POINT a, POINT b) { long dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; }

/* follow the real mouse; leave pad mode when someone moves it physically */
static void track_real_cursor(void)
{
    POINT real;
    if (!real_GetCursorPos(&real)) return;
    if (g_pad_mode) {
        int moved = dist2(real, g_last_real) > 0;
        if (moved && dist2(real, g_commanded) > 4 && dist2(real, g_anchor) > 4 && !is_echo(real, gi_now_ms())) {
            g_pad_mode = 0;
            set_override(0);
            LOG2("cursor: real mouse moved to (%ld,%ld), pad mode off", real.x, real.y);
        }
    }
    g_last_real = real;
    if (!g_pad_mode) {
        POINT c = real;
        ScreenToClient(g_hwnd, &c);
        g_vx = (float)c.x; g_vy = (float)c.y;
        g_virt_screen = pack_pt(real);
        if (g_cfg.override_mode == OVERRIDE_ALWAYS) g_override_active = 1;
    }
}

/* where the game thinks the mouse is, in client coordinates */
static POINT game_cursor_client(void)
{
    POINT p;
    if (g_pad_mode && g_override_active) p = g_commanded;
    else real_GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    return p;
}

/* ---------- calibration: game UI coords <-> client pixels ---------- */
#define CAL_N 8
typedef struct { double p[CAL_N], v[CAL_N]; int n; double a, b; int ok; } Axis;
static Axis g_cal_x, g_cal_y;

static void axis_reset(Axis *A) { memset(A, 0, sizeof *A); }

static void axis_fit(Axis *A)
{
    A->ok = 0;
    if (A->n < 2) return;
    double mn = A->p[0], mx = A->p[0];
    for (int i = 1; i < A->n; i++) { if (A->p[i] < mn) mn = A->p[i]; if (A->p[i] > mx) mx = A->p[i]; }
    if (mx - mn < 40) return;
    double sp = 0, sv = 0, spp = 0, spv = 0, n = A->n;
    for (int i = 0; i < A->n; i++) { sp += A->p[i]; sv += A->v[i]; spp += A->p[i] * A->p[i]; spv += A->p[i] * A->v[i]; }
    double den = n * spp - sp * sp;
    if (fabs(den) < 1e-9) return;
    double a = (n * spv - sp * sv) / den, b = (sv - a * sp) / n;
    if (fabs(a) < 1e-4) return;
    double worst = 0;
    for (int i = 0; i < A->n; i++) { double r = fabs(A->v[i] - (a * A->p[i] + b)) / fabs(a); if (r > worst) worst = r; }
    if (worst > 3.0 && A->n > 2) {
        /* mapping changed (resize, fullscreen...): keep the two newest samples */
        A->p[0] = A->p[A->n - 2]; A->v[0] = A->v[A->n - 2];
        A->p[1] = A->p[A->n - 1]; A->v[1] = A->v[A->n - 1];
        A->n = 2;
        axis_fit(A);
        return;
    }
    A->a = a; A->b = b; A->ok = 1;
}

static void axis_add(Axis *A, double p, double v)
{
    for (int i = 0; i < A->n; i++) {
        if (fabs(A->p[i] - p) < 6) { /* replace, move to newest */
            for (int j = i; j < A->n - 1; j++) { A->p[j] = A->p[j + 1]; A->v[j] = A->v[j + 1]; }
            A->n--;
            break;
        }
    }
    if (A->n == CAL_N) {
        for (int j = 0; j < CAL_N - 1; j++) { A->p[j] = A->p[j + 1]; A->v[j] = A->v[j + 1]; }
        A->n--;
    }
    A->p[A->n] = p; A->v[A->n] = v; A->n++;
    axis_fit(A);
}

static POINT g_stable_pos;
static uint32_t g_stable_frame;
static double g_stable_time;
static int g_stable_sampled;

static int sample_ready(uint32_t sample_frame, uint32_t since_frame, double since_time, double now)
{
    if (hooks_have_frames()) return (int32_t)(sample_frame - since_frame) >= 2 && now - since_time > 25.0;
    return now - since_time > 120.0;
}

static void passive_sample(double now)
{
    POINT p = game_cursor_client();
    if (p.x != g_stable_pos.x || p.y != g_stable_pos.y) {
        g_stable_pos = p; g_stable_frame = hooks_frame(); g_stable_time = now; g_stable_sampled = 0;
        return;
    }
    if (g_stable_sampled) return;
    double gx, gy; uint32_t f;
    if (!hooks_mouse_sample(&gx, &gy, &f) || !sample_ready(f, g_stable_frame, g_stable_time, now)) return;
    if (p.x < 0 || p.y < 0 || p.x >= g_cw || p.y >= g_ch) { g_stable_sampled = 1; return; }
    int was = g_cal_x.ok && g_cal_y.ok;
    axis_add(&g_cal_x, p.x, gx);
    axis_add(&g_cal_y, p.y, gy);
    g_stable_sampled = 1;
    LOG2("calib sample: client (%ld,%ld) -> game (%.2f,%.2f)", p.x, p.y, gx, gy);
    if (!was && g_cal_x.ok && g_cal_y.ok)
        LOG1("calibrated (passive): game = %.4f*x%+.2f, %.4f*y%+.2f (client %dx%d)",
             g_cal_x.a, g_cal_x.b, g_cal_y.a, g_cal_y.b, g_cw, g_ch);
}

static int calibrate_active(void)
{
    double t_all = gi_now_ms();
    float pts[2][2] = { { g_cw * 0.38f, g_ch * 0.38f }, { g_cw * 0.62f, g_ch * 0.62f } };
    for (int i = 0; i < 2; i++) {
        move_cursor(pts[i][0], pts[i][1]);
        uint32_t f0 = hooks_frame();
        double t0 = gi_now_ms();
        for (;;) {
            Sleep(4);
            double now = gi_now_ms(), gx, gy; uint32_t f;
            if (hooks_mouse_sample(&gx, &gy, &f) && sample_ready(f, f0, t0, now)) {
                POINT c = game_cursor_client();
                axis_add(&g_cal_x, c.x, gx);
                axis_add(&g_cal_y, c.y, gy);
                break;
            }
            if (now - t0 > 600) { LOG1("calibration: game did not report the mouse (timeout)"); return 0; }
        }
    }
    LOG1("calibrated (active, %.0f ms): game = %.4f*x%+.2f, %.4f*y%+.2f (client %dx%d)%s",
         gi_now_ms() - t_all, g_cal_x.a, g_cal_x.b, g_cal_y.a, g_cal_y.b, g_cw, g_ch,
         g_cal_x.ok && g_cal_y.ok ? "" : " - FAILED");
    return g_cal_x.ok && g_cal_y.ok;
}

/* ---------- smart navigation ---------- */
#define MAX_TARGETS 768
typedef struct { float x, y, w, h; } Target;

static int collect_targets(Target *t, int max)
{
    static NavRect rs[MAX_TARGETS];
    double age;
    int n = hooks_snapshot(rs, MAX_TARGETS, &age);
    int m = 0;
    float area_max = g_cfg.max_area * (float)g_cw * (float)g_ch;
    for (int i = 0; i < n && m < max; i++) {
        float x = (float)((rs[i].cx - g_cal_x.b) / g_cal_x.a);
        float y = (float)((rs[i].cy - g_cal_y.b) / g_cal_y.a);
        float w = (float)fabs(rs[i].w / g_cal_x.a), h = (float)fabs(rs[i].h / g_cal_y.a);
        if (w < g_cfg.min_size_px || h < g_cfg.min_size_px) continue;
        if (w * h > area_max) continue;
        if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) continue;
        int dup = 0;
        for (int j = 0; j < m; j++) {
            if (fabsf(t[j].x - x) <= 3 && fabsf(t[j].y - y) <= 3) {
                if (w * h < t[j].w * t[j].h) { t[j].w = w; t[j].h = h; }
                dup = 1; break;
            }
        }
        if (!dup) t[m++] = (Target){ x, y, w, h };
    }
    return m;
}

static const float k_dir[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }; /* up down left right */
static const char *const k_dir_name[4] = { "up", "down", "left", "right" };

/* 1 = moved to a target, 0 = no targets (caller may step instead), -1 = targets but none that way */
static int smart_nav(int dir)
{
    float px = g_vx, py = g_vy, dx = k_dir[dir][0], dy = k_dir[dir][1];
    if (!(g_cal_x.ok && g_cal_y.ok)) {
        int ok = calibrate_active();
        move_cursor(px, py); /* calibration moved the cursor around: put it back */
        if (!ok) return 0;
    }
    static Target t[MAX_TARGETS];
    int n = collect_targets(t, MAX_TARGETS);
    if (n == 0) { LOG2("nav %s: no targets on screen", k_dir_name[dir]); return 0; }
    int best = -1;
    float best_score = 1e30f;
    for (int i = 0; i < n; i++) {
        float vx = t[i].x - px, vy = t[i].y - py;
        float along = vx * dx + vy * dy;
        float perp = fabsf(vx * dy - vy * dx);
        if (along <= 3.0f) continue;
        float score = along + perp * g_cfg.perp_weight;
        if (perp > along * 2.5f) score += 1e6f; /* far off to the side: last resort */
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best < 0) { LOG2("nav %s: %d targets, none in that direction", k_dir_name[dir], n); return -1; }
    LOG2("nav %s: %d targets, (%.0f,%.0f) -> (%.0f,%.0f) size %.0fx%.0f", k_dir_name[dir], n,
         px, py, t[best].x, t[best].y, t[best].w, t[best].h);
    move_cursor(t[best].x, t[best].y);
    return 1;
}

static int g_nav_logged;
static void do_nav(int dir)
{
    if (g_cfg.nav_smart && hooks_have_rects()) {
        int r = smart_nav(dir);
        if (r > 0 && !g_nav_logged++) LOG1("first smart navigation ok");
        if (r != 0) return;
    }
    float step = g_cfg.nav_step * (float)g_ch;
    move_cursor(g_vx + k_dir[dir][0] * step, g_vy + k_dir[dir][1] * step);
}

/* ---------- actions ---------- */
static int g_down[ACT_COUNT];
static double g_next_repeat[ACT_COUNT];

static void release_all(void)
{
    if (g_down[ACT_CLICK]) send_mouse(MOUSEEVENTF_LEFTUP, 0);
    if (g_down[ACT_RCLICK]) send_mouse(MOUSEEVENTF_RIGHTUP, 0);
    if (g_down[ACT_ESC]) send_key(VK_ESCAPE, 1);
    memset(g_down, 0, sizeof g_down);
}

static LONG g_latched[256]; /* presses seen by the game this tick */
static void take_latches(void)
{
    for (int i = 0; i < 256; i++)
        g_latched[i] = g_key_latch[i] ? InterlockedExchange(&g_key_latch[i], 0) : 0;
}
static int key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static int key_down(int vk) { return key_held(vk) || g_latched[vk & 0xFF]; }

static int repeats(int a)
{
    return a == ACT_WHEEL_UP || a == ACT_WHEEL_DOWN ||
           a == ACT_NAV_UP || a == ACT_NAV_DOWN || a == ACT_NAV_LEFT || a == ACT_NAV_RIGHT;
}

static void fire(int a, int pressed)
{
    switch (a) {
    case ACT_CLICK:  send_mouse(pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0); break;
    case ACT_RCLICK: send_mouse(pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0); break;
    case ACT_ESC:    send_key(VK_ESCAPE, !pressed); break;
    case ACT_CENTER: if (pressed) move_cursor(g_cw * 0.5f, g_ch * 0.5f); break;
    case ACT_WHEEL_UP:   if (pressed) send_mouse(MOUSEEVENTF_WHEEL, (DWORD)WHEEL_DELTA); break;
    case ACT_WHEEL_DOWN: if (pressed) send_mouse(MOUSEEVENTF_WHEEL, (DWORD)-WHEEL_DELTA); break;
    case ACT_NAV_UP: case ACT_NAV_DOWN: case ACT_NAV_LEFT: case ACT_NAV_RIGHT:
        if (pressed) do_nav(a - ACT_NAV_UP);
        break;
    default: break;
    }
}

static float stick_curve(float x, float y, float dz, float *ox, float *oy)
{
    float mag = sqrtf(x * x + y * y);
    if (mag <= dz) { *ox = *oy = 0; return 0; }
    if (mag > 1) mag = 1;
    float s = (mag - dz) / (1 - dz);
    s = powf(s, 1.7f);
    float k = s / sqrtf(x * x + y * y);
    *ox = x * k; *oy = y * k;
    return s;
}

/* ---------- main loop ---------- */
static DWORD WINAPI input_thread(LPVOID arg)
{
    (void)arg;
    g_pid = GetCurrentProcessId();
    if (g_cfg.pad_enabled) load_xinput();

    double last = gi_now_ms(), next_window_check = 0, kb_hold_start = 0, scroll_acc = 0, stats_at = 0;
    int was_active = 0, logged_window = 0;

    for (;;) {
        Sleep(5);
        double now = gi_now_ms();
        float dt = (float)((now - last) / 1000.0);
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        if (now >= next_window_check) {
            next_window_check = now + 1000;
            if (!g_hwnd || !IsWindow(g_hwnd)) {
                g_hwnd = find_game_window();
                if (g_hwnd && !logged_window++) LOG1("game window found");
            }
        }
        if (!g_hwnd) continue;

        RECT cr;
        if (!GetClientRect(g_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) continue;
        if (cr.right != g_cw || cr.bottom != g_ch) {
            if (g_cw) LOG1("client size %dx%d -> %ldx%ld, recalibrating", g_cw, g_ch, cr.right, cr.bottom);
            g_cw = cr.right; g_ch = cr.bottom;
            axis_reset(&g_cal_x); axis_reset(&g_cal_y);
        }

        int active = GetForegroundWindow() == g_hwnd || g_cfg.background_input;
        Pad pad;
        read_pad(&pad, now);
        if (!g_cfg.pad_enabled) memset(&pad, 0, sizeof pad);

        if (!active) {
            take_latches();
            if (was_active) release_all();
            was_active = 0;
            if (g_pad_mode) { g_pad_mode = 0; set_override(0); }
            continue;
        }
        was_active = 1;
        track_real_cursor();
        passive_sample(now);

        /* keyboard is read only while the game has focus; Alt combos belong to the game */
        take_latches();
        int kb = g_cfg.kb_enabled && GetForegroundWindow() == g_hwnd && !key_held(VK_MENU);
        int free_mod = kb && g_cfg.free_move_mod && key_held(g_cfg.free_move_mod);
        float kbx = 0, kby = 0;

        int now_down[ACT_COUNT];
        for (int a = 0; a < ACT_COUNT; a++) {
            int d = (pad.buttons & g_cfg.pad_map[a]) != 0;
            int k = 0;
            if (kb) for (int i = 0; i < MAX_KEYS_PER_ACT && g_cfg.key_map[a][i]; i++) k |= key_down(g_cfg.key_map[a][i]);
            if (k && free_mod && a >= ACT_NAV_UP && a <= ACT_NAV_RIGHT) {
                kbx += k_dir[a - ACT_NAV_UP][0]; kby += k_dir[a - ACT_NAV_UP][1];
                k = 0;
            }
            now_down[a] = d | k;
        }

        for (int a = 0; a < ACT_COUNT; a++) {
            if (now_down[a] && !g_down[a]) {
                g_down[a] = 1;
                g_next_repeat[a] = now + g_cfg.repeat_delay_ms;
                LOG2("action %s down", g_act_names[a]);
                fire(a, 1);
            } else if (!now_down[a] && g_down[a]) {
                g_down[a] = 0;
                fire(a, 0);
            } else if (now_down[a] && repeats(a) && now >= g_next_repeat[a]) {
                g_next_repeat[a] = now + g_cfg.repeat_interval_ms;
                fire(a, 1);
            }
        }

        /* free cursor movement */
        float mul = 1.0f;
        if (g_down[ACT_SLOW]) mul *= g_cfg.slow_mul;
        if (g_down[ACT_FAST]) mul *= g_cfg.fast_mul;
        float mx = 0, my = 0;
        if (g_cfg.stick_cursor != STICK_NONE) {
            float sx = g_cfg.stick_cursor == STICK_LEFT ? pad.lx : pad.rx;
            float sy = g_cfg.stick_cursor == STICK_LEFT ? pad.ly : pad.ry;
            float ox, oy;
            if (stick_curve(sx, sy, g_cfg.deadzone, &ox, &oy) > 0) {
                float speed = g_cfg.cursor_speed * (float)g_ch * mul;
                mx += ox * speed * dt;
                my -= oy * speed * dt;
            }
        }
        if (kbx != 0 || kby != 0) {
            if (kb_hold_start == 0) kb_hold_start = now;
            float ramp = 0.35f + 0.65f * fminf(1.0f, (float)((now - kb_hold_start) / 600.0));
            float len = sqrtf(kbx * kbx + kby * kby);
            float speed = g_cfg.kb_cursor_speed * (float)g_ch * ramp * mul;
            mx += kbx / len * speed * dt;
            my += kby / len * speed * dt;
        } else {
            kb_hold_start = 0;
        }
        if (mx != 0 || my != 0) move_cursor(g_vx + mx, g_vy + my);

        /* scroll stick */
        if (g_cfg.stick_scroll != STICK_NONE) {
            float sy = g_cfg.stick_scroll == STICK_LEFT ? pad.ly : pad.ry;
            float sx = 0, ox, oy;
            if (stick_curve(sx, sy, g_cfg.deadzone, &ox, &oy) > 0) {
                scroll_acc += oy * g_cfg.scroll_rate * dt;
                while (scroll_acc >= 1) { send_mouse(MOUSEEVENTF_WHEEL, (DWORD)WHEEL_DELTA); scroll_acc -= 1; }
                while (scroll_acc <= -1) { send_mouse(MOUSEEVENTF_WHEEL, (DWORD)-WHEEL_DELTA); scroll_acc += 1; }
            } else {
                scroll_acc = 0;
            }
        }

        if (g_log_level >= 2 && now >= stats_at) {
            stats_at = now + 5000;
            NavRect tmp[4]; double age;
            int nr = hooks_snapshot(tmp, 4, &age);
            LOG2("stats: frame %u, rects(last frame) %s%d, age %.0f ms, calib %s, pad %s, pad_mode %d, override %ld",
                 hooks_frame(), nr >= 4 ? ">=" : "", nr, age > 1e6 ? -1.0 : age, g_cal_x.ok && g_cal_y.ok ? "ok" : "no",
                 pad.connected ? "yes" : "no", g_pad_mode, (long)g_override_active);
        }
    }
    return 0;
}

void input_start(void)
{
    HANDLE th = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else LOG1("input thread: CreateThread failed (%lu)", GetLastError());
}
