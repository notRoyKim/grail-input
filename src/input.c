/*
 * input.c - input thread: gamepad (XInput) + keyboard -> virtual mouse
 */
#include "common.h"
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
static int g_last_dev_pad;        /* last input came from the gamepad: show pad labels */
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
            g_last_dev_pad = 0;
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

/* both axes fitted, increasing, and with about the same scale (the game never stretches) */
static int cal_ok(void)
{
    if (!(g_cal_x.ok && g_cal_y.ok)) return 0;
    if (g_cal_x.a <= 0 || g_cal_y.a <= 0) return 0;
    double r = g_cal_x.a / g_cal_y.a;
    return r > 0.7 && r < 1.43;
}

static void cal_reset(const char *why)
{
    if (g_cal_x.n || g_cal_y.n) LOG1("calibration reset: %s", why);
    axis_reset(&g_cal_x); axis_reset(&g_cal_y);
}

static POINT g_stable_pos;
static uint32_t g_stable_frame;
static double g_stable_time;
static int g_stable_sampled;

/* a mouse sample the game took at least one frame after `since_frame` and kept for two frames */
static int settled_sample(uint32_t since_frame, double since_time, double now, double *gx, double *gy)
{
    uint32_t f;
    if (hooks_have_frames())
        return hooks_mouse_stable(gx, gy, &f) && (int32_t)(f - since_frame) >= 1 && now - since_time > 25.0;
    return hooks_mouse_sample(gx, gy, &f) && now - since_time > 150.0;
}

static void passive_sample(double now)
{
    POINT p = game_cursor_client();
    if (p.x != g_stable_pos.x || p.y != g_stable_pos.y) {
        g_stable_pos = p; g_stable_frame = hooks_frame(); g_stable_time = now; g_stable_sampled = 0;
        return;
    }
    if (g_stable_sampled) return;
    double gx, gy;
    if (!settled_sample(g_stable_frame, g_stable_time, now, &gx, &gy)) return;
    g_stable_sampled = 1;
    if (p.x < 0 || p.y < 0 || p.x >= g_cw || p.y >= g_ch) return;
    if (cal_ok()) {
        double ex = fabs(g_cal_x.a * p.x + g_cal_x.b - gx), ey = fabs(g_cal_y.a * p.y + g_cal_y.b - gy);
        if (ex > 4 || ey > 4) cal_reset("game coordinates changed");
    }
    int was = cal_ok();
    axis_add(&g_cal_x, p.x, gx);
    axis_add(&g_cal_y, p.y, gy);
    LOG2("calib sample: client (%ld,%ld) -> game (%.2f,%.2f)", p.x, p.y, gx, gy);
    if (!was && cal_ok())
        LOG1("calibrated (passive): game = %.4f*x%+.2f, %.4f*y%+.2f (client %dx%d)",
             g_cal_x.a, g_cal_x.b, g_cal_y.a, g_cal_y.b, g_cw, g_ch);
}

static int calibrate_active(void)
{
    static const float pts[3][2] = { { 0.36f, 0.36f }, { 0.64f, 0.64f }, { 0.50f, 0.42f } };
    for (int attempt = 0; attempt < 3; attempt++) {
        double t_all = gi_now_ms();
        axis_reset(&g_cal_x); axis_reset(&g_cal_y);
        int got = 0;
        for (int i = 0; i < 3; i++) {
            move_cursor(pts[i][0] * g_cw, pts[i][1] * g_ch);
            uint32_t f0 = hooks_frame();
            double t0 = gi_now_ms(), gx = 0, gy = 0;
            int ok = 0;
            while (gi_now_ms() - t0 < 700) {
                Sleep(4);
                if (settled_sample(f0, t0, gi_now_ms(), &gx, &gy)) { ok = 1; break; }
            }
            if (!ok) break;
            POINT c = game_cursor_client();
            axis_add(&g_cal_x, c.x, gx);
            axis_add(&g_cal_y, c.y, gy);
            LOG2("calib point %d: client (%ld,%ld) -> game (%.2f,%.2f)", i, c.x, c.y, gx, gy);
            got++;
        }
        /* all three points must sit on one straight line per axis */
        if (got == 3 && g_cal_x.n == 3 && g_cal_y.n == 3 && cal_ok()) {
            LOG1("calibrated (active, %.0f ms): game = %.4f*x%+.2f, %.4f*y%+.2f (client %dx%d)",
                 gi_now_ms() - t_all, g_cal_x.a, g_cal_x.b, g_cal_y.a, g_cal_y.b, g_cw, g_ch);
            return 1;
        }
        LOG1("calibration attempt %d inconsistent (%d points), retrying", attempt + 1, got);
        Sleep(150);
    }
    cal_reset("active calibration failed");
    return 0;
}

/* ---------- smart navigation ---------- */
#define MAX_TARGETS 768
typedef struct { float x, y, w, h; uint16_t site; uint8_t keep, dead; } Target;

static float overlap(const Target *a, const Target *b)
{
    float x0 = fmaxf(a->x - a->w / 2, b->x - b->w / 2), x1 = fminf(a->x + a->w / 2, b->x + b->w / 2);
    float y0 = fmaxf(a->y - a->h / 2, b->y - b->h / 2), y1 = fminf(a->y + a->h / 2, b->y + b->h / 2);
    return (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
}

static int contains_center(const Target *a, const Target *b)
{
    return fabsf(b->x - a->x) < a->w / 2 && fabsf(b->y - a->y) < a->h / 2;
}

/* The game also tests the mouse against things that never react when hovered:
 * scroll areas, panels around a list of cards, the same spot tested twice...
 * Keep only what is likely to be a button, card, icon or tooltip spot. */
static int collect_targets(Target *t, int max)
{
    static NavRect rs[MAX_TARGETS];
    double age;
    int n = hooks_snapshot(rs, MAX_TARGETS, &age);
    int m = 0;
    float W = (float)g_cw, H = (float)g_ch, area_max = g_cfg.max_area * W * H;
    for (int i = 0; i < n && m < max; i++) {
        const SiteInfo *si = hooks_site(rs[i].site);
        if (si->ignored) continue;
        float x = (float)((rs[i].cx - g_cal_x.b) / g_cal_x.a);
        float y = (float)((rs[i].cy - g_cal_y.b) / g_cal_y.a);
        float w = (float)fabs(rs[i].w / g_cal_x.a), h = (float)fabs(rs[i].h / g_cal_y.a);
        if (w < g_cfg.min_size_px || h < g_cfg.min_size_px) continue;
        if (w * h > area_max) continue;
        /* mostly on screen */
        float vx = fminf(x + w / 2, W) - fmaxf(x - w / 2, 0), vy = fminf(y + h / 2, H) - fmaxf(y - h / 2, 0);
        if (vx <= 0 || vy <= 0 || vx * vy < 0.5f * w * h) continue;
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        t[m++] = (Target){ x, y, w, h, rs[i].site, si->keep, 0 };
    }
    /* the same spot tested more than once: keep the smaller, more specific one */
    for (int i = 0; i < m; i++) {
        if (t[i].dead) continue;
        for (int j = i + 1; j < m; j++) {
            if (t[j].dead) continue;
            float ai = t[i].w * t[i].h, aj = t[j].w * t[j].h, ov = overlap(&t[i], &t[j]);
            if (ov >= 0.6f * (ai + aj - ov)) {
                if (aj < ai) { t[i].dead = 1; break; }
                t[j].dead = 1;
            }
        }
    }
    /* panels that hold other targets (hand area, list backgrounds) */
    if (g_cfg.drop_containers) {
        for (int i = 0; i < m; i++) {
            if (t[i].dead || t[i].keep) continue;
            float ai = t[i].w * t[i].h;
            int inside = 0;
            for (int j = 0; j < m; j++) {
                if (j == i || t[j].dead) continue;
                if (t[j].w * t[j].h < 0.6f * ai && contains_center(&t[i], &t[j])) inside++;
            }
            if ((inside >= 1 && ai > 0.06f * W * H) || inside >= 4) t[i].dead = 1;
        }
    }
    int k = 0;
    for (int i = 0; i < m; i++) if (!t[i].dead) t[k++] = t[i];
    return k;
}

static double g_last_dump;
static void dump_targets(const Target *t, int n)
{
    double now = gi_now_ms();
    if (g_log_level < 2 || now - g_last_dump < 3000) return;
    g_last_dump = now;
    LOG2("targets on screen (%d):", n);
    for (int i = 0; i < n; i++)
        LOG2("  (%4.0f,%4.0f) %4.0fx%-4.0f %s", t[i].x, t[i].y, t[i].w, t[i].h, hooks_site(t[i].site)->desc);
}

static const float k_dir[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }; /* up down left right */
static const char *const k_dir_name[4] = { "up", "down", "left", "right" };

/* 1 = moved to a target, 0 = no targets (caller may step instead), -1 = targets but none that way */
static int smart_nav(int dir)
{
    float px = g_vx, py = g_vy, dx = k_dir[dir][0], dy = k_dir[dir][1];
    if (!cal_ok()) {
        int ok = calibrate_active();
        move_cursor(px, py); /* calibration moved the cursor around: put it back */
        if (!ok) return 0;
    }
    static Target t[MAX_TARGETS];
    int n = collect_targets(t, MAX_TARGETS);
    if (n == 0) { LOG2("nav %s: no targets on screen", k_dir_name[dir]); return 0; }
    dump_targets(t, n);
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
    LOG2("nav %s: %d targets, (%.0f,%.0f) -> (%.0f,%.0f) size %.0fx%.0f [%s]", k_dir_name[dir], n,
         px, py, t[best].x, t[best].y, t[best].w, t[best].h, hooks_site(t[best].site)->desc);
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

/* ---------- overlay state and shortcuts ---------- */
static OverlayState g_ov;
static Target g_tt[MAX_TARGETS];        /* targets of this tick */
static int g_ntt;

static void toast(const char *msg)
{
    snprintf(g_ov.toast, sizeof g_ov.toast, "%s", msg);
    g_ov.toast_until = gi_now_ms() + 1800;
    LOG1("toast: %s", msg);
}

static void refresh_targets(void)
{
    g_ntt = cal_ok() ? collect_targets(g_tt, MAX_TARGETS) : 0;
}

/* smallest target under a client point */
static int target_at(float x, float y)
{
    int best = -1;
    float ba = 1e30f;
    for (int i = 0; i < g_ntt; i++) {
        if (fabsf(g_tt[i].x - x) <= g_tt[i].w / 2 && fabsf(g_tt[i].y - y) <= g_tt[i].h / 2) {
            float a = g_tt[i].w * g_tt[i].h;
            if (a < ba) { ba = a; best = i; }
        }
    }
    return best;
}

/* the button a slot was bound to, if it is on screen now */
static int match_slot(const Slot *s)
{
    int best = -1;
    float bd = 1e30f;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        for (int i = 0; i < g_ntt; i++) {
            const SiteInfo *si = hooks_site(g_tt[i].site);
            if (strcmp(si->script, s->script) != 0) continue;
            if (pass == 0 && si->off != s->off) continue;
            float dx = g_tt[i].x / g_cw - s->nx, dy = g_tt[i].y / g_ch - s->ny, d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
    }
    if (best >= 0 && bd > 0.2f * 0.2f) return -1; /* same code, but somewhere else entirely */
    return best;
}

static const char *slot_label(const Slot *s, char *buf, int cap)
{
    if (s->pad && (g_last_dev_pad || !s->key)) return pad_name(s->pad);
    if (s->key) return vk_name(s->key, buf, cap);
    return "?";
}

static void slot_title(int i, char *out, int cap)
{
    char kb[24];
    const Slot *s = &g_slots[i];
    if (s->key && s->pad) snprintf(out, (size_t)cap, "%s/%s", vk_name(s->key, kb, sizeof kb), pad_name(s->pad));
    else if (s->key) snprintf(out, (size_t)cap, "%s", vk_name(s->key, kb, sizeof kb));
    else if (s->pad) snprintf(out, (size_t)cap, "%s", pad_name(s->pad));
    else snprintf(out, (size_t)cap, "SLOT %d", i + 1);
}

static int ensure_calibrated(void)
{
    if (cal_ok()) return 1;
    float px = g_vx, py = g_vy;
    int ok = calibrate_active();
    move_cursor(px, py);
    return ok;
}

static void bind_slot(int i)
{
    char title[48], msg[64];
    slot_title(i, title, sizeof title);
    if (!ensure_calibrated()) { toast("NOT READY - TRY AGAIN"); return; }
    refresh_targets();
    POINT c = game_cursor_client();
    int t = target_at((float)c.x, (float)c.y);
    Slot *s = &g_slots[i];
    if (t < 0) {
        s->bound = 0;
        snprintf(msg, sizeof msg, "%s CLEARED", title);
    } else {
        const SiteInfo *si = hooks_site(g_tt[t].site);
        snprintf(s->script, sizeof s->script, "%s", si->script);
        s->off = si->off;
        s->nx = g_tt[t].x / g_cw; s->ny = g_tt[t].y / g_ch;
        s->bound = 1;
        snprintf(msg, sizeof msg, "%s SET", title);
        LOG1("shortcut %d -> %s +0x%x at (%.3f, %.3f)", i + 1, si->desc, s->off, s->nx, s->ny);
    }
    shortcuts_save();
    toast(msg);
}

/* hold the bind button + press a pad button: that button's slot (or a free one) */
static void pad_bind(uint32_t bit)
{
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS && slot < 0; i++) if (g_slots[i].pad == bit) slot = i;
    for (int i = 0; i < MAX_SLOTS && slot < 0; i++) if (!g_slots[i].pad) { slot = i; g_slots[i].pad = bit; }
    if (slot < 0) { toast("NO FREE SLOT"); return; }
    bind_slot(slot);
}

/* a shortcut presses its button: move there, then click a couple of frames later */
static struct { int stage; uint32_t f; double t; } g_click;

static void run_slot(int i)
{
    char title[48], msg[64];
    slot_title(i, title, sizeof title);
    if (!g_slots[i].bound) {
        char kb[24];
        if (g_slots[i].key) snprintf(msg, sizeof msg, "%s: NOT SET - CTRL+%s ON A BUTTON", title, vk_name(g_slots[i].key, kb, sizeof kb));
        else snprintf(msg, sizeof msg, "%s: NOT SET", title);
        toast(msg);
        return;
    }
    if (g_click.stage || !ensure_calibrated()) return;
    refresh_targets();
    int t = match_slot(&g_slots[i]);
    if (t < 0) { snprintf(msg, sizeof msg, "%s: NOT ON THIS SCREEN", title); toast(msg); return; }
    LOG2("shortcut %d -> (%.0f,%.0f) %s", i + 1, g_tt[t].x, g_tt[t].y, hooks_site(g_tt[t].site)->desc);
    move_cursor(g_tt[t].x, g_tt[t].y);
    g_click.stage = 1; g_click.f = hooks_frame(); g_click.t = gi_now_ms();
}

static void click_tick(double now)
{
    if (!g_click.stage) return;
    int frames = hooks_have_frames() ? (int)(hooks_frame() - g_click.f) : (int)((now - g_click.t) / 17.0);
    if (frames < 2 && now - g_click.t < 400) return;
    send_mouse(g_click.stage == 1 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0);
    g_click.stage = g_click.stage == 1 ? 2 : 0;
    g_click.f = hooks_frame(); g_click.t = now;
}

static void overlay_tick(void)
{
    if (!g_cfg.overlay) return;
    refresh_targets();
    g_ov.client_w = g_cw; g_ov.client_h = g_ch;
    g_ov.highlight_on = 0;
    if (g_pad_mode && g_ntt) {
        int t = target_at(g_vx, g_vy);
        if (t >= 0) {
            g_ov.highlight_on = 1;
            g_ov.highlight = (ORect){ g_tt[t].x, g_tt[t].y, g_tt[t].w, g_tt[t].h };
        }
    }
    g_ov.nprompts = 0;
    for (int i = 0; i < MAX_SLOTS && g_ov.nprompts < OV_MAX_PROMPTS; i++) {
        if (!g_slots[i].bound) continue;
        int t = match_slot(&g_slots[i]);
        if (t < 0) continue;
        OPrompt *p = &g_ov.prompts[g_ov.nprompts++];
        p->r = (ORect){ g_tt[t].x, g_tt[t].y, g_tt[t].w, g_tt[t].h };
        char kb[24];
        snprintf(p->label, sizeof p->label, "%s", slot_label(&g_slots[i], kb, sizeof kb));
    }
    overlay_publish(&g_ov);
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
static uint32_t g_prev_pad, g_prev_act, g_pad_suppress;
static int g_mod_was, g_mod_combo;
static int g_slotkey_prev[MAX_SLOTS];
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
                if (g_hwnd && !logged_window++) {
                    LOG1("game window found");
                    if (g_cfg.overlay) overlay_start(g_hwnd);
                }
            }
        }
        if (!g_hwnd) continue;

        RECT cr;
        if (!GetClientRect(g_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) continue;
        if (cr.right != g_cw || cr.bottom != g_ch) {
            if (g_cw) LOG1("client size %dx%d -> %ldx%ld, recalibrating", g_cw, g_ch, cr.right, cr.bottom);
            g_cw = cr.right; g_ch = cr.bottom;
            cal_reset("window resized");
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

        /* gamepad: hold the bind button + another button binds a shortcut,
         * a short tap of the bind button keeps its normal action (center) */
        uint32_t raw = pad.buttons, newly = raw & ~g_prev_pad, bmod = g_cfg.bind_mod;
        int mod_held = bmod && (raw & bmod);
        if (raw || fabsf(pad.lx) > g_cfg.deadzone || fabsf(pad.ly) > g_cfg.deadzone) g_last_dev_pad = 1;
        if (mod_held) {
            if (!g_mod_was) g_mod_combo = 0;
            for (uint32_t bit = 1; bit && bit <= PAD_RT; bit <<= 1)
                if ((newly & ~bmod) & bit) { pad_bind(bit); g_mod_combo = 1; }
        } else if (g_mod_was) {
            if (!g_mod_combo && (g_cfg.pad_map[ACT_CENTER] & bmod)) fire(ACT_CENTER, 1);
            g_pad_suppress = raw; /* buttons still held from the combo stay quiet until released */
        }
        g_mod_was = mod_held;
        g_pad_suppress &= raw;
        g_prev_pad = raw;
        uint32_t act = mod_held ? 0 : (raw & ~bmod & ~g_pad_suppress);
        uint32_t act_newly = act & ~g_prev_act;
        g_prev_act = act;
        for (int i = 0; i < MAX_SLOTS; i++)
            if (g_slots[i].pad && (act_newly & g_slots[i].pad)) run_slot(i);

        /* keyboard: a slot key runs its shortcut, Ctrl + the slot key binds it */
        for (int i = 0; i < MAX_SLOTS; i++) {
            int vk = g_slots[i].key;
            int k = kb && vk && key_down(vk);
            if (k && !g_slotkey_prev[i]) {
                int ctrl = g_latched[vk & 0xFF] ? (g_latched[vk & 0xFF] & 2) != 0 : key_held(VK_CONTROL);
                g_last_dev_pad = 0;
                if (ctrl) bind_slot(i); else run_slot(i);
            }
            g_slotkey_prev[i] = k;
        }
        click_tick(now);

        int now_down[ACT_COUNT];
        for (int a = 0; a < ACT_COUNT; a++) {
            int d = (act & g_cfg.pad_map[a]) != 0;
            int k = 0;
            if (kb) for (int i = 0; i < MAX_KEYS_PER_ACT && g_cfg.key_map[a][i]; i++) k |= key_down(g_cfg.key_map[a][i]);
            if (k) g_last_dev_pad = 0;
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

        overlay_tick();

        if (g_log_level >= 2 && now >= stats_at) {
            stats_at = now + 5000;
            NavRect tmp[4]; double age;
            int nr = hooks_snapshot(tmp, 4, &age);
            LOG2("stats: frame %u, rects(last frame) %s%d, age %.0f ms, calib %s, pad %s, pad_mode %d, override %ld",
                 hooks_frame(), nr >= 4 ? ">=" : "", nr, age > 1e6 ? -1.0 : age, cal_ok() ? "ok" : "no",
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
