/* osk.h — what the on-screen keyboard needs from C (docs/ON-SCREEN-KEYBOARD.md), with no dependencies
 * so tests/osk_test.c covers it: is a device a keyboard someone can type on, which keys a tap may
 * press, and the short queue of taps. Never stores or prints what is typed: a queued tap is a key
 * code that leaves the queue at the next pass of the main loop. */
#ifndef LORD_OSK_H
#define LORD_OSK_H

#include <stddef.h>

/* level of a tap: which modifiers the layout needs for that symbol */
#define OSK_LEVEL_SHIFT 1
#define OSK_LEVEL_ALTGR 2

#define OSK_QUEUE_MAX 16

typedef struct { int code, level; } OskTap;

/* The keys with a label on the keyboard, in a fixed order (lord.input.keyLabels). */
extern const unsigned short osk_label_codes[];
extern const size_t osk_label_count;

/* Does the keyboard have this key? Only keys that type or move the caret: no Ctrl, Alt, Super or
 * function keys, so a tap can't be a shortcut. Modifiers come only from the level. */
int osk_key_allowed(int code);
int osk_level_valid(int level);

/* 1 if it fits, 0 if the code or level is refused or the queue is full. */
int osk_push(int code, int level);
int osk_pop(OskTap *t);
void osk_clear(void);

/* key_bit(code) of a device's EV_KEY capabilities: letters, space, Enter, Backspace and Shift.
 * Power buttons, media keys and remotes don't have them. */
int osk_full_keyboard(const unsigned long *keys, size_t nlongs, const char *name);

/* Does a keyboard count as attached? bus: input_id.bustype; portable: this computer is a laptop;
 * typed: a letter key was pressed on it. A PC's PS/2 controller reports a keyboard even with nothing
 * plugged in, so on a desktop that one counts only once it has typed. */
int osk_keyboard_counts(int bus, int portable, int typed);

/* /sys/class/dmi/id/chassis_type: does this kind of computer carry its own keyboard? */
int osk_chassis_portable(int type);

#endif
