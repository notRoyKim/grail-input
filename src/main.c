/*
 * main.c - version.dll proxy, logging, config, DllMain
 */
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

/* ---------- version.dll proxy: forward the 17 exports to the real DLL ---------- */
#define VER_FUNCS(X) \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define DECL_PTR(n) void *p_##n = NULL;
VER_FUNCS(DECL_PTR)

#define STUB(n) __asm__(".text\n.globl proxy_" #n "\nproxy_" #n ":\n\tjmp *p_" #n "(%rip)\n");
VER_FUNCS(STUB)

static HMODULE g_real_version;
static void load_real_version(void)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 20) return;
    wcscat(path, L"\\version.dll");
    g_real_version = LoadLibraryW(path);
    if (!g_real_version) return;
#define RESOLVE(n) p_##n = (void *)GetProcAddress(g_real_version, #n);
    VER_FUNCS(RESOLVE)
}

/* ---------- time ---------- */
static LARGE_INTEGER g_qpf;
double gi_now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_qpf.QuadPart;
}

/* ---------- logging ---------- */
int g_log_level = 1;
static wchar_t g_log_path[MAX_PATH];
static CRITICAL_SECTION g_log_cs;
static int g_log_lines;

void gi_log(int level, const char *fmt, ...)
{
    if (level > g_log_level || !g_log_path[0]) return;
    EnterCriticalSection(&g_log_cs);
    if (g_log_lines < 20000) {
        FILE *f = _wfopen(g_log_path, L"a");
        if (f) {
            SYSTEMTIME t; GetLocalTime(&t);
            fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
            fputc('\n', f);
            fclose(f);
            g_log_lines++;
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

/* ---------- config ---------- */
GiConfig g_cfg;
const char *const g_act_names[ACT_COUNT] = {
    "click", "right_click", "escape", "center",
    "wheel_up", "wheel_down",
    "nav_up", "nav_down", "nav_left", "nav_right",
    "slow", "fast",
};

typedef struct { const char *name; uint32_t bit; } NameBit;
static const NameBit k_pad_names[] = {
    {"DUP", 0x0001}, {"DDOWN", 0x0002}, {"DLEFT", 0x0004}, {"DRIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"SELECT", 0x0020}, {"VIEW", 0x0020}, {"MENU", 0x0010},
    {"LS", 0x0040}, {"L3", 0x0040}, {"RS", 0x0080}, {"R3", 0x0080},
    {"LB", 0x0100}, {"L1", 0x0100}, {"RB", 0x0200}, {"R1", 0x0200},
    {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
    {"LT", PAD_LT}, {"L2", PAD_LT}, {"RT", PAD_RT}, {"R2", PAD_RT},
};

static const NameBit k_vk_names[] = {
    {"BACKSPACE", VK_BACK}, {"BACK", VK_BACK}, {"TAB", VK_TAB}, {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN},
    {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU},
    {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"LCONTROL", VK_LCONTROL}, {"RCONTROL", VK_RCONTROL},
    {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
    {"PAUSE", VK_PAUSE}, {"CAPSLOCK", VK_CAPITAL}, {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE},
    {"SPACE", VK_SPACE}, {"PRIOR", VK_PRIOR}, {"PAGEUP", VK_PRIOR}, {"NEXT", VK_NEXT}, {"PAGEDOWN", VK_NEXT},
    {"END", VK_END}, {"HOME", VK_HOME}, {"LEFT", VK_LEFT}, {"UP", VK_UP}, {"RIGHT", VK_RIGHT}, {"DOWN", VK_DOWN},
    {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"DEL", VK_DELETE}, {"APPS", VK_APPS}, {"MENUKEY", VK_APPS},
    {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7},
    {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9}, {"MULTIPLY", VK_MULTIPLY}, {"ADD", VK_ADD},
    {"SUBTRACT", VK_SUBTRACT}, {"DECIMAL", VK_DECIMAL}, {"DIVIDE", VK_DIVIDE},
    {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
    {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
};

static int parse_vk(const char *s)
{
    if (!*s) return 0;
    for (size_t i = 0; i < sizeof k_vk_names / sizeof *k_vk_names; i++)
        if (_stricmp(s, k_vk_names[i].name) == 0) return (int)k_vk_names[i].bit;
    if (s[1] == 0 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (s[1] == 0 && s[0] >= 'a' && s[0] <= 'z') return s[0] - 'a' + 'A';
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, NULL, 16);
    return -1;
}

static uint32_t parse_pad_bit(const char *s)
{
    for (size_t i = 0; i < sizeof k_pad_names / sizeof *k_pad_names; i++)
        if (_stricmp(s, k_pad_names[i].name) == 0) return k_pad_names[i].bit;
    return 0;
}

/* split "A, START" into tokens; calls fn on each trimmed token */
static void for_tokens(const char *list, void (*fn)(const char *, void *), void *ctx)
{
    char tok[64];
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '+') p++;
        int n = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && n < 63) tok[n++] = *p++;
        tok[n] = 0;
        while (*p && *p != ',') p++;
        if (n) fn(tok, ctx);
    }
}

/* tiny ini reader: UTF-8 (with or without BOM) or UTF-16LE with BOM,
 * so Korean comments never depend on the system code page */
#define INI_MAX 160
typedef struct { char sec[32], key[48], val[160]; } IniEntry;
static IniEntry g_ini_e[INI_MAX];
static int g_ini_n;

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static void ini_parse(const wchar_t *path)
{
    g_ini_n = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, NULL), got = 0;
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) { CloseHandle(h); return; }
    char *buf = HeapAlloc(GetProcessHeap(), 0, size + 2);
    if (!buf) { CloseHandle(h); return; }
    ReadFile(h, buf, size, &got, NULL);
    CloseHandle(h);
    buf[got] = 0; buf[got + 1] = 0;
    char *text = buf;
    if (got >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, (wchar_t *)(buf + 2), (int)(got - 2) / 2, NULL, 0, NULL, NULL);
        text = HeapAlloc(GetProcessHeap(), 0, (size_t)n + 1);
        if (!text) { HeapFree(GetProcessHeap(), 0, buf); return; }
        WideCharToMultiByte(CP_UTF8, 0, (wchar_t *)(buf + 2), (int)(got - 2) / 2, text, n, NULL, NULL);
        text[n] = 0;
    } else if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        text = buf + 3;
    }
    char sec[32] = "";
    for (char *line = text; line && *line;) {
        char *next = strpbrk(line, "\r\n");
        if (next) { *next++ = 0; while (*next == '\r' || *next == '\n') next++; }
        char *l = trim(line);
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (e) { *e = 0; snprintf(sec, sizeof sec, "%s", trim(l + 1)); }
        } else if (*l && *l != ';' && *l != '#') {
            char *eq = strchr(l, '=');
            if (eq && g_ini_n < INI_MAX) {
                *eq = 0;
                char *v = eq + 1;
                for (char *c = v; *c; c++) if (*c == ';' || *c == '#') { *c = 0; break; }
                IniEntry *e = &g_ini_e[g_ini_n++];
                snprintf(e->sec, sizeof e->sec, "%s", sec);
                snprintf(e->key, sizeof e->key, "%s", trim(l));
                snprintf(e->val, sizeof e->val, "%s", trim(v));
            }
        }
        line = next;
    }
    if (text != buf && !(text == buf + 3)) HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, buf);
}

static void ini_str(const char *sec, const char *key, const char *def, char *out, int cap)
{
    for (int i = g_ini_n - 1; i >= 0; i--) {
        if (_stricmp(g_ini_e[i].sec, sec) == 0 && _stricmp(g_ini_e[i].key, key) == 0) {
            snprintf(out, (size_t)cap, "%s", g_ini_e[i].val);
            return;
        }
    }
    snprintf(out, (size_t)cap, "%s", def);
}
static int ini_int(const char *sec, const char *key, int def)
{
    char d[32], b[64]; snprintf(d, sizeof d, "%d", def);
    ini_str(sec, key, d, b, sizeof b);
    return b[0] ? atoi(b) : def;
}
static float ini_float(const char *sec, const char *key, float def)
{
    char d[32], b[64]; snprintf(d, sizeof d, "%g", def);
    ini_str(sec, key, d, b, sizeof b);
    return b[0] ? (float)atof(b) : def;
}

static void add_pad(const char *tok, void *ctx)
{
    uint32_t *m = ctx;
    if (_stricmp(tok, "NONE") == 0) return;
    uint32_t b = parse_pad_bit(tok);
    if (!b) LOG1("config: unknown gamepad button '%s'", tok);
    *m |= b;
}
typedef struct { int *keys; int n; } KeyCtx;
static void add_key(const char *tok, void *ctx)
{
    KeyCtx *k = ctx;
    if (_stricmp(tok, "NONE") == 0) return;
    int vk = parse_vk(tok);
    if (vk <= 0) { LOG1("config: unknown key '%s'", tok); return; }
    if (k->n < MAX_KEYS_PER_ACT) k->keys[k->n++] = vk;
}

static const char *const k_pad_defaults[ACT_COUNT] = {
    "A", "B", "X, START", "BACK",
    "LB", "RB",
    "DUP", "DDOWN", "DLEFT", "DRIGHT",
    "LT", "RT",
};
static const char *const k_key_defaults[ACT_COUNT] = {
    "RETURN, SPACE", "RCONTROL, APPS", "NONE", "HOME",
    "PAGEUP", "PAGEDOWN",
    "UP", "DOWN", "LEFT", "RIGHT",
    "NONE", "NONE",
};

static int parse_stick(const char *s, int def)
{
    if (_stricmp(s, "left") == 0) return STICK_LEFT;
    if (_stricmp(s, "right") == 0) return STICK_RIGHT;
    if (_stricmp(s, "none") == 0 || _stricmp(s, "off") == 0) return STICK_NONE;
    return def;
}

void config_load(const wchar_t *ini_path)
{
    char b[256];
    ini_parse(ini_path);
    GiConfig *c = &g_cfg;
    memset(c, 0, sizeof *c);

    c->enabled = ini_int("general", "enabled", 1);
    g_log_level = ini_int("general", "log_level", 1);
    c->background_input = ini_int("general", "background_input", 0);

    c->pad_enabled = ini_int("gamepad", "enabled", 1);
    for (int a = 0; a < ACT_COUNT; a++) {
        ini_str("gamepad", g_act_names[a], k_pad_defaults[a], b, sizeof b);
        for_tokens(b, add_pad, &c->pad_map[a]);
    }
    ini_str("gamepad", "cursor_stick", "left", b, sizeof b); c->stick_cursor = parse_stick(b, STICK_LEFT);
    ini_str("gamepad", "scroll_stick", "right", b, sizeof b); c->stick_scroll = parse_stick(b, STICK_RIGHT);
    c->deadzone = ini_float("gamepad", "deadzone", 0.22f);
    c->cursor_speed = ini_float("gamepad", "cursor_speed", 1.1f);
    c->slow_mul = ini_float("gamepad", "slow_multiplier", 0.35f);
    c->fast_mul = ini_float("gamepad", "fast_multiplier", 2.0f);
    c->scroll_rate = ini_float("gamepad", "scroll_rate", 12.0f);

    c->kb_enabled = ini_int("keyboard", "enabled", 1);
    for (int a = 0; a < ACT_COUNT; a++) {
        ini_str("keyboard", g_act_names[a], k_key_defaults[a], b, sizeof b);
        KeyCtx k = { c->key_map[a], 0 };
        for_tokens(b, add_key, &k);
    }
    ini_str("keyboard", "free_move_modifier", "SHIFT", b, sizeof b);
    c->free_move_mod = parse_vk(b);
    if (c->free_move_mod < 0) c->free_move_mod = 0;
    c->kb_cursor_speed = ini_float("keyboard", "cursor_speed", 0.7f);

    c->nav_smart = ini_int("navigation", "smart", 1);
    c->repeat_delay_ms = ini_int("navigation", "repeat_delay_ms", 320);
    c->repeat_interval_ms = ini_int("navigation", "repeat_interval_ms", 120);
    c->nav_step = ini_float("navigation", "fallback_step", 0.08f);
    c->min_size_px = ini_float("navigation", "min_target_size", 6.0f);
    c->max_area = ini_float("navigation", "max_target_area", 0.45f);
    c->perp_weight = ini_float("navigation", "sideways_weight", 2.0f);

    ini_str("cursor", "parsec_fix", "off", b, sizeof b);
    if (_stricmp(b, "transparent") == 0 || _stricmp(b, "on") == 0 || strcmp(b, "1") == 0) c->cursorfix_mode = CURSORFIX_TRANSPARENT;
    else if (_stricmp(b, "arrow") == 0) c->cursorfix_mode = CURSORFIX_ARROW;
    else c->cursorfix_mode = CURSORFIX_OFF;
    ini_str("cursor", "position_override", "auto", b, sizeof b);
    if (_stricmp(b, "never") == 0 || _stricmp(b, "off") == 0) c->override_mode = OVERRIDE_NEVER;
    else if (_stricmp(b, "always") == 0) c->override_mode = OVERRIDE_ALWAYS;
    else c->override_mode = OVERRIDE_AUTO;
}

/* ---------- DllMain ---------- */
static int under_wine(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    QueryPerformanceFrequency(&g_qpf);
    InitializeCriticalSection(&g_log_cs);
    load_real_version();

    wchar_t dir[MAX_PATH], ini[MAX_PATH];
    DWORD n = GetModuleFileNameW(inst, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return TRUE;
    slash[1] = 0;
    if (wcslen(dir) + 32 >= MAX_PATH) return TRUE;
    wcscpy(g_log_path, dir); wcscat(g_log_path, L"grail_input.log");
    wcscpy(ini, dir); wcscat(ini, L"grail_input.ini");
    DeleteFileW(g_log_path); /* fresh log every run */

    config_load(ini);

    wchar_t exe[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    LOG1("grail-input %s loaded into %ls", GI_VERSION, exe);
    LOG1("real version.dll: %s, wine/proton: %s, config: %s",
         g_real_version ? "ok" : "FAILED", under_wine() ? "yes" : "no",
         GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES ? "grail_input.ini" : "defaults (no ini)");

    /* only touch the game itself, never other programs that might load this dll */
    const wchar_t *base = wcsrchr(exe, L'\\');
    base = base ? base + 1 : exe;
    if (_wcsicmp(base, L"grail.exe") != 0) {
        LOG1("not grail.exe (%ls), staying passive", base);
        return TRUE;
    }

    if (g_log_level >= 2) {
        for (int a = 0; a < ACT_COUNT; a++)
            LOG2("config %-11s pad=0x%05x keys=%d,%d,%d,%d", g_act_names[a], g_cfg.pad_map[a],
                 g_cfg.key_map[a][0], g_cfg.key_map[a][1], g_cfg.key_map[a][2], g_cfg.key_map[a][3]);
        LOG2("config sticks cursor=%d scroll=%d deadzone=%.2f speed=%.2f nav_smart=%d parsec_fix=%d override=%d",
             g_cfg.stick_cursor, g_cfg.stick_scroll, g_cfg.deadzone, g_cfg.cursor_speed, g_cfg.nav_smart,
             g_cfg.cursorfix_mode, g_cfg.override_mode);
    }
    hooks_install_iat();
    if (!g_cfg.enabled) {
        LOG1("input mod disabled in ini ([general] enabled=0)");
        return TRUE;
    }
    hooks_install();
    input_start();
    return TRUE;
}
