/* keygrab.h — the keys the desktop keeps from a focused Wayland app (see keygrab.c). */
#ifndef LORD_KEYGRAB_H
#define LORD_KEYGRAB_H

enum { KG_SHIFT = 1, KG_CTRL = 2, KG_ALT = 4, KG_META = 8 };
enum { KG_PASS = 0, KG_TAKE = 1, KG_HOLD = 2 };

/* The shell picks one of the fixed tables: "" (none), "flow" or "classic". 1 ok; 0 for any other
 * text, and the table in use does not change. */
int keygrab_set(const char *profile);

/* The key's letter, 'a'..'z': what the active layout says, or the US position on a layout without
 * Latin letters; 0 if it is not a letter. base: KeyInfo.base. */
char keygrab_letter(int code, const char *base);

/* What the table in use does with this key and exactly these modifiers: KG_PASS, KG_TAKE (the shell
 * gets it, the app never does) or KG_HOLD (the same, and every other key goes to the shell too
 * until the modifier that started it is released: the app switcher). */
int keygrab_match(int code, char letter, unsigned mods);

/* Does this ❖ + letter reach Linux apps as Ctrl (Lord Flow's table)? */
int keygrab_is_remap(char letter, unsigned mods);

/* The keys on a keyboard's media row: always LordOS's, whatever has the keyboard. 1 if code is one. */
int keygrab_system_key(int code);

#endif
