/*
 * gml_hooks.c - hooks inside grail.exe
 *
 * grail is a GameMaker YYC build: every GML script is native code, and the
 * exe keeps a table of { "gml_Script_name", function, ... } entries.  We find
 * a few scripts by name in that table and hook them with MinHook:
 *
 *   mouse_on(x, y, w, h)              -> hover test of a centred rectangle
 *   mouse_on_between(x1, y1, x2, y2)  -> hover test of a corner rectangle
 *   mouse_get_x() / mouse_get_y()     -> mouse in the game's UI coordinates
 *   oGameManager Step event           -> one call per frame (frame boundary)
 *
 * Every rectangle the game tests the mouse against during a frame is a
 * button, card, map node, tooltip area... = a navigation target.
 *
 * The exe's import table is also patched for USER32!SetCursor (Parsec cursor
 * fix) and USER32!GetCursorPos (virtual cursor position override, needed
 * when the OS refuses to warp the pointer, e.g. some gamescope setups).
 */
#include "common.h"
#include "MinHook.h"
#include <string.h>

/* ---------- GameMaker runtime value ---------- */
typedef struct {
    union { double real; int64_t i64; int32_t i32; void *ptr; } v;
    uint32_t flags;
    uint32_t kind;
} RValue;

enum { VALUE_REAL = 0, VALUE_INT32 = 7, VALUE_INT64 = 10, VALUE_BOOL = 13 };

static int rv_num(const RValue *r, double *out)
{
    if (!r) return 0;
    switch (r->kind & 0x00FFFFFF) {
    case VALUE_REAL: case VALUE_BOOL: *out = r->v.real; return 1;
    case VALUE_INT32: *out = r->v.i32; return 1;
    case VALUE_INT64: *out = (double)r->v.i64; return 1;
    }
    return 0;
}

typedef RValue *(*ScriptFn)(void *self, void *other, RValue *result, int argc, RValue **args);
typedef void *(*EventFn)(void *self, void *other, void *a3, void *a4);

/* ---------- locate a gml_Script_* function by name ---------- */
typedef struct { BYTE *lo, *hi; } Range;
static Range g_text, g_img;
static Range g_data[8]; static int g_ndata;

static void scan_sections(void)
{
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    g_img.lo = base; g_img.hi = base + nt->OptionalHeader.SizeOfImage;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        BYTE *lo = base + s->VirtualAddress;
        DWORD vs = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (!g_text.lo) { g_text.lo = lo; g_text.hi = lo + vs; }
        } else if ((s->Characteristics & IMAGE_SCN_MEM_WRITE) && g_ndata < 8) {
            /* only the initialised part can hold the static table */
            DWORD raw = s->SizeOfRawData < vs ? s->SizeOfRawData : vs;
            g_data[g_ndata].lo = lo; g_data[g_ndata].hi = lo + raw; g_ndata++;
        }
    }
}

/* find every NUL-delimited copy of `name` in the read-only parts of the image */
static int find_strings(const char *name, BYTE **out, int max)
{
    size_t len = strlen(name) + 1; /* include terminator */
    int n = 0;
    BYTE *base = g_img.lo;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (s->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) continue;
        if (!(s->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        BYTE *lo = base + s->VirtualAddress;
        DWORD sz = s->SizeOfRawData < s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
        BYTE *hi = lo + sz;
        for (BYTE *p = lo + 1; p + len <= hi; p++) {
            p = memchr(p, name[0], (size_t)(hi - p));
            if (!p || p + len > hi) break;
            if (p[-1] == 0 && memcmp(p, name, len) == 0 && n < max) out[n++] = p;
        }
    }
    return n;
}

static void *find_gml_function(const char *name)
{
    BYTE *strs[8];
    int ns = find_strings(name, strs, 8);
    if (!ns) return NULL;
    for (int d = 0; d < g_ndata; d++) {
        for (BYTE *p = (BYTE *)(((uintptr_t)g_data[d].lo + 7) & ~(uintptr_t)7); p + 16 <= g_data[d].hi; p += 8) {
            uintptr_t v = *(uintptr_t *)p;
            for (int k = 0; k < ns; k++) {
                if (v != (uintptr_t)strs[k]) continue;
                BYTE *fn = *(BYTE **)(p + 8);
                if (fn >= g_text.lo && fn < g_text.hi) return fn;
            }
        }
    }
    return NULL;
}

/* ---------- navigation target store ---------- */
#define MAX_RECTS 768
static CRITICAL_SECTION g_cs;
static NavRect g_collect[MAX_RECTS]; static int g_ncollect;
static NavRect g_pub[MAX_RECTS];     static int g_npub;
static double g_pub_time = -1e9, g_collect_time;
static volatile LONG g_frame;
static int g_have_step, g_have_rects;

/* latest mouse position as the game sees it (UI coordinates) */
static volatile double g_mx, g_my;
static volatile LONG g_m_frame;
static volatile LONG g_have_mx, g_have_my;

/* caller holds g_cs */
static void publish_locked(double now)
{
    memcpy(g_pub, g_collect, sizeof(NavRect) * (size_t)g_ncollect);
    g_npub = g_ncollect;
    g_pub_time = now;
    g_ncollect = 0;
}

static void add_rect(float cx, float cy, float w, float h)
{
    if (!(w > 0.5f && h > 0.5f) || w > 1e5f || h > 1e5f) return;
    if (cx != cx || cy != cy) return; /* NaN */
    double now = gi_now_ms();
    EnterCriticalSection(&g_cs);
    /* without the frame hook a pause between calls marks a new frame */
    if (!g_have_step && g_ncollect && now - g_collect_time > 4.0) publish_locked(g_collect_time);
    int dup = 0;
    for (int i = g_ncollect - 1; i >= 0 && i >= g_ncollect - 64; i--) {
        NavRect *r = &g_collect[i];
        if (r->cx == cx && r->cy == cy && r->w == w && r->h == h) { dup = 1; break; }
    }
    if (!dup && g_ncollect < MAX_RECTS)
        g_collect[g_ncollect++] = (NavRect){ cx, cy, w, h };
    g_collect_time = now;
    LeaveCriticalSection(&g_cs);
}

static ScriptFn o_mouse_on, o_mouse_on_between, o_mouse_get_x, o_mouse_get_y;
static EventFn o_step;

static RValue *hk_mouse_on(void *self, void *other, RValue *res, int argc, RValue **args)
{
    double x, y, w, h;
    if (argc >= 4 && args && rv_num(args[0], &x) && rv_num(args[1], &y) &&
        rv_num(args[2], &w) && rv_num(args[3], &h)) {
        if (w < 0) w = -w;
        if (h < 0) h = -h;
        add_rect((float)x, (float)y, (float)w, (float)h);
    }
    return o_mouse_on(self, other, res, argc, args);
}

static RValue *hk_mouse_on_between(void *self, void *other, RValue *res, int argc, RValue **args)
{
    double x1, y1, x2, y2;
    if (argc >= 4 && args && rv_num(args[0], &x1) && rv_num(args[1], &y1) &&
        rv_num(args[2], &x2) && rv_num(args[3], &y2)) {
        double w = x2 - x1, h = y2 - y1;
        if (w < 0) w = -w;
        if (h < 0) h = -h;
        add_rect((float)((x1 + x2) * 0.5), (float)((y1 + y2) * 0.5), (float)w, (float)h);
    }
    return o_mouse_on_between(self, other, res, argc, args);
}

static RValue *hk_mouse_get_x(void *self, void *other, RValue *res, int argc, RValue **args)
{
    RValue *r = o_mouse_get_x(self, other, res, argc, args);
    double v;
    if (rv_num(r, &v)) { g_mx = v; g_m_frame = g_frame; g_have_mx = 1; }
    return r;
}

static RValue *hk_mouse_get_y(void *self, void *other, RValue *res, int argc, RValue **args)
{
    RValue *r = o_mouse_get_y(self, other, res, argc, args);
    double v;
    if (rv_num(r, &v)) { g_my = v; g_m_frame = g_frame; g_have_my = 1; }
    return r;
}

/* Step event of the game manager: runs once per frame before drawing.
 * Publish what the previous frame tested, start collecting a new frame. */
static void *hk_step(void *self, void *other, void *a3, void *a4)
{
    EnterCriticalSection(&g_cs);
    publish_locked(gi_now_ms());
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_frame);
    return o_step(self, other, a3, a4);
}

uint32_t hooks_frame(void) { return (uint32_t)g_frame; }
int hooks_have_frames(void) { return g_have_step && g_frame > 2; }
int hooks_have_rects(void) { return g_have_rects; }

int hooks_snapshot(NavRect *out, int max, double *age_ms)
{
    double now = gi_now_ms();
    EnterCriticalSection(&g_cs);
    if (!g_have_step && g_ncollect && now - g_collect_time > 4.0) publish_locked(g_collect_time);
    int n = g_npub < max ? g_npub : max;
    memcpy(out, g_pub, sizeof(NavRect) * (size_t)n);
    *age_ms = now - g_pub_time;
    LeaveCriticalSection(&g_cs);
    if (*age_ms > 250.0) n = 0; /* nothing tested recently: stale */
    return n;
}

int hooks_mouse_sample(double *gx, double *gy, uint32_t *frame)
{
    if (!g_have_mx || !g_have_my) return 0;
    *gx = g_mx; *gy = g_my; *frame = (uint32_t)g_m_frame;
    return 1;
}

static int hook_one(const char *name, void *detour, void **orig)
{
    void *fn = find_gml_function(name);
    if (!fn) { LOG1("hook %s: not found", name); return 0; }
    MH_STATUS st = MH_CreateHook(fn, detour, orig);
    if (st != MH_OK) { LOG1("hook %s: MH_CreateHook failed (%s)", name, MH_StatusToString(st)); return 0; }
    st = MH_EnableHook(fn);
    if (st != MH_OK) { LOG1("hook %s: MH_EnableHook failed (%s)", name, MH_StatusToString(st)); return 0; }
    LOG1("hook %s: ok (+0x%llx)", name, (unsigned long long)((BYTE *)fn - g_img.lo));
    return 1;
}

int hooks_install(void)
{
    InitializeCriticalSection(&g_cs);
    scan_sections();
    if (!g_text.lo || !g_ndata) { LOG1("hooks: could not read exe sections"); return 0; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG1("hooks: MH_Initialize failed (%s)", MH_StatusToString(st));
        return 0;
    }
    int mask = 0;
    double t0 = gi_now_ms();
    if (hook_one("gml_Script_mouse_on", (void *)hk_mouse_on, (void **)&o_mouse_on)) mask |= 1;
    if (hook_one("gml_Script_mouse_on_between", (void *)hk_mouse_on_between, (void **)&o_mouse_on_between)) mask |= 2;
    if (hook_one("gml_Script_mouse_get_x", (void *)hk_mouse_get_x, (void **)&o_mouse_get_x)) mask |= 4;
    if (hook_one("gml_Script_mouse_get_y", (void *)hk_mouse_get_y, (void **)&o_mouse_get_y)) mask |= 8;
    if (hook_one("gml_Object_oGameManager_Step_0", (void *)hk_step, (void **)&o_step)) mask |= 16;
    g_have_rects = (mask & 3) && (mask & 12) == 12;
    g_have_step = (mask & 16) != 0;
    LOG1("hooks: installed mask=0x%x in %.1f ms (smart navigation %s)", mask, gi_now_ms() - t0,
         g_have_rects ? "available" : "UNAVAILABLE - falling back to plain cursor");
    return mask;
}

/* ---------- import table hooks: SetCursor (Parsec fix), GetCursorPos ---------- */
static HCURSOR (WINAPI *p_SetCursor)(HCURSOR);
static BOOL (WINAPI *p_GetCursorPos)(LPPOINT);
static HCURSOR g_replacement;
volatile LONG g_override_active;
volatile LONG64 g_virt_screen;

BOOL real_GetCursorPos(POINT *p)
{
    return p_GetCursorPos ? p_GetCursorPos(p) : GetCursorPos(p);
}

static HCURSOR make_replacement(void)
{
    if (g_cfg.cursorfix_mode == CURSORFIX_ARROW)
        return LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    /* 32x32 cursor with AND=1, XOR=0 everywhere: draws nothing */
    BYTE and_mask[32 * 32 / 8], xor_mask[32 * 32 / 8];
    memset(and_mask, 0xFF, sizeof and_mask);
    memset(xor_mask, 0x00, sizeof xor_mask);
    return CreateCursor(GetModuleHandleW(NULL), 0, 0, 32, 32, and_mask, xor_mask);
}

static HCURSOR WINAPI hk_SetCursor(HCURSOR h)
{
    if (h == NULL && g_cfg.cursorfix_mode != CURSORFIX_OFF) {
        if (!g_replacement) g_replacement = make_replacement();
        h = g_replacement;
    }
    return p_SetCursor(h);
}

/* Key presses seen by the game's message loop.  Polling GetAsyncKeyState alone
 * can miss very short taps, so the input thread also consumes these latches. */
volatile LONG g_key_latch[256];
static BOOL (WINAPI *p_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);

static void latch_key(const MSG *m)
{
    UINT vk = (UINT)(m->wParam & 0xFF);
    g_key_latch[vk] = 1;
    UINT ext = (m->lParam & 0x01000000) != 0;
    if (vk == VK_CONTROL) g_key_latch[ext ? VK_RCONTROL : VK_LCONTROL] = 1;
    else if (vk == VK_MENU) g_key_latch[ext ? VK_RMENU : VK_LMENU] = 1;
    else if (vk == VK_SHIFT) {
        UINT sc = (UINT)((m->lParam >> 16) & 0xFF);
        UINT lr = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
        if (lr == VK_LSHIFT || lr == VK_RSHIFT) g_key_latch[lr] = 1;
    }
}

static BOOL WINAPI hk_PeekMessageW(LPMSG m, HWND h, UINT f1, UINT f2, UINT rm)
{
    BOOL r = p_PeekMessageW(m, h, f1, f2, rm);
    /* first press only: auto-repeat messages (bit 30) handled late could fire stray actions */
    if (r && m && (rm & PM_REMOVE) && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) &&
        !(m->lParam & (1L << 30)))
        latch_key(m);
    return r;
}

static BOOL WINAPI hk_GetCursorPos(LPPOINT p)
{
    BOOL r = p_GetCursorPos(p);
    if (r && p && g_override_active) {
        LONG64 v = g_virt_screen;
        p->x = (LONG)(int32_t)(v & 0xFFFFFFFF);
        p->y = (LONG)(int32_t)(v >> 32);
    }
    return r;
}

static void **iat_slot(const char *dll, const char *fn)
{
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char *)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn) == 0) return (void **)&slots->u1.Function;
        }
    }
    return NULL;
}

static int patch_slot(void **slot, void *fn)
{
    DWORD old;
    if (!slot || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

void hooks_install_iat(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    p_SetCursor = (void *)GetProcAddress(u32, "SetCursor");
    p_GetCursorPos = (void *)GetProcAddress(u32, "GetCursorPos");
    p_PeekMessageW = (void *)GetProcAddress(u32, "PeekMessageW");

    if (g_cfg.cursorfix_mode != CURSORFIX_OFF) {
        int ok = p_SetCursor && patch_slot(iat_slot("user32.dll", "SetCursor"), (void *)hk_SetCursor);
        LOG1("parsec cursor fix (%s): %s", g_cfg.cursorfix_mode == CURSORFIX_ARROW ? "arrow" : "transparent",
             ok ? "ok" : "FAILED");
    }
    if (g_cfg.enabled && g_cfg.override_mode != OVERRIDE_NEVER) {
        int ok = p_GetCursorPos && patch_slot(iat_slot("user32.dll", "GetCursorPos"), (void *)hk_GetCursorPos);
        LOG1("cursor position override (%s): %s", g_cfg.override_mode == OVERRIDE_ALWAYS ? "always" : "auto",
             ok ? "ok" : "FAILED");
    }
    if (g_cfg.enabled && g_cfg.kb_enabled) {
        int ok = p_PeekMessageW && patch_slot(iat_slot("user32.dll", "PeekMessageW"), (void *)hk_PeekMessageW);
        LOG1("key press latch: %s", ok ? "ok" : "FAILED (very short key taps may be missed)");
    }
}
