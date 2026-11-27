/*
 * shortcuts.c - shortcut slots: a key / pad button that clicks a chosen game button
 *
 * Slots are bound in game (Ctrl + the slot key, or hold BACK and press a pad
 * button, while the cursor is on a game button) and kept in
 * grail_input_shortcuts.ini next to the dll.  A slot remembers the hover test
 * that found the button (game script + offset of the call) and where it was
 * on screen, so buttons drawn by the same code (the four card piles) are told
 * apart by position.
 */
#include "common.h"
#include <stdio.h>
#include <string.h>

Slot g_slots[MAX_SLOTS];

static const char *const k_default_pad[MAX_SLOTS] = { "LT", "RT", "NONE", "NONE", "NONE", "NONE", "NONE", "NONE" };

static void path(wchar_t *out)
{
    wcscpy(out, g_mod_dir);
    wcscat(out, L"grail_input_shortcuts.ini");
}

void shortcuts_load(void)
{
    wchar_t p[MAX_PATH];
    path(p);
    ini_parse(p);
    int bound = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot *s = &g_slots[i];
        char sec[32], b[256], def_key[8];
        memset(s, 0, sizeof *s);
        snprintf(sec, sizeof sec, "shortcut%d", i + 1);
        snprintf(def_key, sizeof def_key, "F%d", i + 1);
        ini_str(sec, "pad", k_default_pad[i], b, sizeof b);
        s->pad = _stricmp(b, "NONE") == 0 ? 0 : parse_pad_bit(b);
        ini_str(sec, "key", def_key, b, sizeof b);
        s->key = _stricmp(b, "NONE") == 0 ? 0 : parse_vk(b);
        if (s->key < 0) s->key = 0;
        ini_str(sec, "target", "", b, sizeof b);
        unsigned off = 0; float x = 0, y = 0;
        char script[64] = "";
        if (b[0] && sscanf(b, "%63[^+ ] + %x @ %f , %f", script, &off, &x, &y) == 4) {
            snprintf(s->script, sizeof s->script, "%s", script);
            s->off = off; s->nx = x; s->ny = y; s->bound = 1;
            bound++;
        } else if (b[0]) {
            LOG1("shortcuts: cannot read target of [%s]: %s", sec, b);
        }
    }
    LOG1("shortcuts: %d of %d slots bound", bound, MAX_SLOTS);
}

void shortcuts_save(void)
{
    wchar_t p[MAX_PATH];
    path(p);
    FILE *f = _wfopen(p, L"wb");
    if (!f) { LOG1("shortcuts: cannot write grail_input_shortcuts.ini"); return; }
    fputs("; grail-input shortcuts - written by the mod when you bind a shortcut in game.\n"
          "; Bind: put the cursor on a game button, then press Ctrl + the slot key (Ctrl+F1...)\n"
          ";       or hold BACK and press a pad button. Doing it over empty space clears the slot.\n"
          "; pad / key: what triggers the slot. target: game script + offset @ position on screen.\n"
          "; 단축키 설정 - 게임 안에서 지정하면 모드가 자동으로 저장합니다.\n"
          "; 지정: 게임 버튼에 커서를 올리고 Ctrl+단축키(Ctrl+F1 등) 또는 BACK을 누른 채 패드 버튼.\n"
          ";       빈 곳에서 하면 그 칸이 비워집니다.\n\n", f);
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot *s = &g_slots[i];
        char kb[32];
        fprintf(f, "[shortcut%d]\npad = %s\nkey = %s\n", i + 1, s->pad ? pad_name(s->pad) : "NONE",
                s->key ? vk_name(s->key, kb, sizeof kb) : "NONE");
        if (s->bound) fprintf(f, "target = %s+%x @ %.4f,%.4f\n", s->script, s->off, s->nx, s->ny);
        fputc('\n', f);
    }
    fclose(f);
    LOG1("shortcuts: saved");
}
