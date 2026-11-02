/* statusitem.h — a third-party app's item by the clock (docs/GO-LINK.md, section 7). An isolated app has no code in the
 * shell, so it describes the item and the shell draws it: its own package's icon, a short title, an alert mark
 * and a menu of plain items. No bytes of the app's reach the shell's JSON unchecked: this reads what the app sent and
 * writes a new text with only the known fields, each one escaped. No I/O, so tests/pkgapp_test.c covers it. */
#ifndef LORD_SEC_STATUSITEM_H
#define LORD_SEC_STATUSITEM_H

#include <stddef.h>

#define STATUS_MENU_MAX 12
#define STATUS_LABEL_MAX 64

/* [a-z0-9-], 1 to 32: an item's id, never text from elsewhere. */
int statusitem_id_ok(const char *id);
/* { icon, title, alert, menu: [{ id, label, off, line }] } → { app, icon, title, alert, menu } in out. app is a
 * validated package id. 0, or -1 if anything isn't as it should be. */
int statusitem_clean(const char *app, const char *json, size_t len, char *out, size_t n);

#endif
