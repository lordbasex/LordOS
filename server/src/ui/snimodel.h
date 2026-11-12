/* snimodel.h — what the tray host (ui/sni.c) reads from Linux apps, without a bus: the registered service in its two
 * forms, the icon pixmap at the bar's size, the texts and a com.canonical.dbusmenu layout as bounded JSON. Each reader
 * takes a DRd over a message vault/dbus.c already validated, and still checks every count and length. Tested by
 * build/sni-test (tests/sni_test.c). */
#ifndef LORD_UI_SNIMODEL_H
#define LORD_UI_SNIMODEL_H

#include <stddef.h>
#include <stdint.h>

#include "../vault/dbus.h"
#include "../hw/jbuf.h"

#define SNI_NAME_MAX   256              /* a bus name or an object path */
#define SNI_TEXT_MAX   128              /* a title, a tooltip or a menu label, in bytes */
#define SNI_PIX_SIDE   256              /* a pixmap wider or taller than this is skipped */
#define SNI_PIX_COUNT  16               /* pixmaps looked at in one property */
#define SNI_PIX_BYTES  (2u << 20)       /* all the pixmaps of one property together; more and none is used */
#define SNI_PIX_LO     22               /* the sizes the bar draws */
#define SNI_PIX_HI     32
#define SNI_MENU_DEPTH 4                /* levels of submenus kept */
#define SNI_MENU_ITEMS 256              /* menu entries kept in one layout */

/* RegisterStatusNotifierItem's argument: an object path (the item is on the sender's connection) or a bus name (the
 * item is at /StatusNotifierItem there). A unique name must be the sender's own. dest and path get where to call; 0 ok. */
int sni_service(const char *service, const char *sender, char dest[SNI_NAME_MAX], char path[SNI_NAME_MAX]);

/* How far a w x h pixmap is from the bar's sizes (lower is better): 22..32 first, the largest of them; then a larger
 * one, the nearest; a smaller one last. */
int sni_pix_score(int w, int h);

/* A picked pixmap: straight RGBA, w * h * 4 bytes (malloc). */
typedef struct { int w, h; uint8_t *rgba; } SniPix;

/* Reads an a(iiay) (IconPixmap, AttentionIconPixmap) and keeps the one nearest the bar's sizes, from network-order
 * ARGB32 to RGBA. Entries with a size out of 1..SNI_PIX_SIDE or bytes that are not exactly w * h * 4 are skipped; past
 * SNI_PIX_COUNT entries or SNI_PIX_BYTES in all, none is kept. 0 with out filled, -1 none (out empty). The reader ends
 * after the array either way, unless the array itself is malformed (r->err). */
int sni_pixmap(DRd *r, SniPix *out);
void sni_pix_free(SniPix *p);

/* One line of text for the bar or a menu: valid UTF-8, no control or bidi formatting characters, at most n - 1 bytes
 * cut on a character. mnemonic: dbusmenu's '_' marks the access key and is dropped ("__" is a '_'). */
void sni_text(const char *in, char *out, size_t n, int mnemonic);

/* GetLayout's (ia{sv}av) root: its children as a JSON array into b,
 *   [{"id":4,"label":"Open","enabled":true,"toggle":"checkmark","on":false,"sub":[…]}, {"sep":true}, …]
 * Hidden entries are left out; labels go through sni_text; past SNI_MENU_DEPTH a submenu is cut and past
 * SNI_MENU_ITEMS entries the rest are left out (still read past). ids (up to max) gets the ids of the entries that can
 * be clicked, *nids their count, and keys (NULL: not wanted) the key of each: a hash of its place in the menu (the
 * index and label of it and of each entry above it), the same for the same entry in a menu the app built again with
 * new ids (fyne.io/systray's apps do, every time they change one label). 0 ok, -1 malformed. */
int sni_menu_json(DRd *r, HBuf *b, int32_t *ids, uint32_t *keys, int max, int *nids);

/* The key of entry id in a table sni_menu_json filled: 0 found, -1 not there. */
int sni_menu_key(const int32_t *ids, const uint32_t *keys, int n, int32_t id, uint32_t *key);
/* The id of the one entry with that key, or -1 (none, or more than one). */
int32_t sni_menu_find(const int32_t *ids, const uint32_t *keys, int n, uint32_t key);

/* com.canonical.dbusmenu's Event for a chosen entry, the body of signature SNI_EVENT_SIG: (id, "clicked", <int32 0>,
 * timestamp), what libdbusmenu, Qt, Ayatana and fyne.io/systray all read as a click. b must be dw_init'ed. */
#define SNI_EVENT_SIG "isvu"
void sni_event_body(DBuf *b, int32_t id, uint32_t timestamp);

#endif
