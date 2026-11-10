/* sni.h — the tray of Linux apps: org.kde.StatusNotifierWatcher and its host on the session bus, so the icons of
 * sync clients, messengers, game launchers and VPNs show with LordOS's status items (see sni.c, sdk/trayitems.jsx). */
#ifndef LORD_UI_SNI_H
#define LORD_UI_SNI_H

#include <stdint.h>

#include "../hw/jbuf.h"

int  sni_start(void);                   /* after dropping privileges; 0 if the connection is up (the name comes later) */
int  sni_fd(void);                      /* for poll; -1 if absent */
int  sni_wants_write(void);             /* there are unsent messages (POLLOUT) */
void sni_dispatch(void);

/* lord.sni (the desktop only, never an isolated app). The items as a JSON array (the same objects as the "sni"
 * event); one item's current pixmap as straight RGBA (NULL when it shows a file or nothing). */
void sni_list(HBuf *b);
const uint8_t *sni_pixels(int id, int *w, int *h);
/* A click on the item at the pointer's (x, y): Activate (SecondaryActivate for the middle button). An item that is
 * only a menu, or whose app has no Activate, gets its menu instead ("sni-menu" event). 0 sent. */
int  sni_activate(int id, int x, int y, int secondary);
/* A right click: 1 the dbusmenu layout was asked for (it comes as the "sni-menu" event), 0 the app shows its own
 * menu (ContextMenu), -1 no such item. */
int  sni_menu(int id, int x, int y);
/* An entry of the last menu shown for that item was chosen. 0 sent, -1 not one of its entries. */
int  sni_menu_clicked(int id, int32_t entry);

#endif
