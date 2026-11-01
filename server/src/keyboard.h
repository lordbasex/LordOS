/* keyboard.h — keyboard layouts with libxkbcommon (see keyboard.c). */
#ifndef LORD_KEYBOARD_H
#define LORD_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char key[32];               /* "Enter", "ArrowUp", "a", "ñ"… (like KeyboardEvent.key) */
    char chr[16];               /* text it types (with compose applied), "" if nothing */
    char base[8];               /* character of the key without Ctrl/Alt (for shortcuts) */
    int shift, ctrl, alt, altgr, meta, caps, dead;
} KeyInfo;

int  kb_set(const char *layouts, const char *variants);   /* "us,latam", ",abnt2" */
int  kb_set_keymap_string(const char *text);              /* app mode: the compositor's keymap */
void kb_reset_state(void);                                /* forget held keys (app mode, the monitor taken away) */
int  kb_ready(void);
const char *kb_layouts(void);
const char *kb_variants(void);
int  kb_group(void);                                      /* active layout */
void kb_set_group(int g);
void kb_reload_compose(void);                             /* after changing the region */
void kb_key(int code, int value, KeyInfo *k);
/* For Wayland: the keymap in XKB format (malloc, NULL if none) and the
 * serialized modifiers. */
char *kb_keymap_string(void);
/* The on-screen keyboard's labels for the active layout. kb_labels_begin() prepares the four levels
 * (0 plain, 1 Shift, 2 AltGr, 3 both; returns 0 without a keymap), kb_label() writes what `code` types
 * at `level` ("" nothing; "dead:´" a dead key) and kb_labels_end() frees them. They work on a copy:
 * the real state, and the keys held down, are not touched. */
int  kb_labels_begin(void);
void kb_label(int code, int level, char *out, size_t n);
void kb_labels_end(void);
int  kb_has_altgr(void);                /* does Right Alt reach a third level in the active layout? */
void kb_mods(uint32_t *depressed, uint32_t *latched, uint32_t *locked, uint32_t *group);

#endif
