/* bt.h — Bluetooth for Settings → Bluetooth: BlueZ (bluetoothd) over the system bus with our
 * own D-Bus client (hw/sysbus.c, vault/dbus.c). No libbluetooth, no bluetoothctl.
 *
 *   - state: adapters and devices from ObjectManager.GetManagedObjects, kept up to date by the
 *     InterfacesAdded / InterfacesRemoved / PropertiesChanged signals (no polling);
 *   - actions: power, discovery, pair (then trust and connect), connect, disconnect, forget; a
 *     paired device the user connects, or that our agent lets in, is trusted too (bt_should_trust);
 *   - pairing agent: lord-server exports org.bluez.Agent1 at /org/lordos/BluetoothAgent
 *     ("KeyboardDisplay") and registers it as the default agent. PIN and passkey requests are
 *     shown by the system (sdk/bluetooth.js) and answered with bt_answer(). Calls to the agent
 *     are only accepted from org.bluez's unique bus name.
 *
 * Audio devices (headphones, speakers) show up in Sound through PipeWire's own BlueZ support.
 * See docs/SETTINGS.md, "Bluetooth". */
#ifndef LORD_HW_BT_H
#define LORD_HW_BT_H

#include <stdint.h>
#include "../vault/dbus.h"

#define BT_MAX_ADAPTERS 4
#define BT_MAX_DEVICES 64
#define BT_MAX_SERVICES 24              /* 16-bit service UUIDs kept per device */

typedef struct {
    char path[96], address[20], alias[96];
    int powered, discoverable, discovering, pairable;
} BtAdapter;

typedef struct {
    char path[128], adapter[96], address[20], name[96], alias[96], icon[48];
    char addr_type[8];                  /* AddressType: "public" or "random" ("" if not reported) */
    uint32_t cls;                       /* Class of Device: only seen over BR/EDR (classic) */
    uint16_t services[BT_MAX_SERVICES]; /* UUIDs on the Bluetooth base UUID, as 16-bit numbers */
    int nservices;
    uint16_t appearance;
    int paired, bonded, trusted, connected, blocked, legacy;
    int rssi, has_rssi;
    int battery;                        /* org.bluez.Battery1 percentage, -1 if none */
    int busy;                           /* pairing/connecting in progress (ours) */
} BtDevice;

typedef struct {
    BtAdapter adapters[BT_MAX_ADAPTERS];
    int nadapters;
    BtDevice devices[BT_MAX_DEVICES];
    int ndevices;
} BtModel;

/* Called with (kind, json): "bt" (the whole state), "bt-prompt" (agent request), "bt-result"
 * (an action finished: {op, path, ok, error}). */
typedef void (*BtEvent)(const char *kind, const char *json);

void bt_start(BtEvent fn);              /* after sysbus_start */
int  bt_available(void);                /* bluetoothd is on the bus */
char *bt_state_json(void);              /* malloc */
/* Actions (async; the result arrives as "bt-result"). 0 if sent. */
int  bt_power(int on);
int  bt_discover(int on);
int  bt_pair(const char *dev_path);
int  bt_connect(const char *dev_path);
int  bt_disconnect(const char *dev_path);
int  bt_forget(const char *dev_path);
/* Answer the pending agent request id: accept, with a PIN (text) or passkey (number) if asked. */
int  bt_answer(int id, int accept, const char *value);
void bt_flush_events(void);             /* once per main-loop turn: sends "bt" if it changed */

/* ---- parsing, exposed for tests/bt_test.c ---- */
BtModel *bt_model(void);
/* Applies an interface's a{sv} properties (r positioned at the array) to the object at path. */
void bt_apply_props(BtModel *mdl, const char *path, const char *iface, DRd *r);
/* GetManagedObjects reply body (a{oa{sa{sv}}}) into the model. 0 if valid. */
int  bt_parse_managed(BtModel *mdl, const DMsg *m);
void bt_remove_object(BtModel *mdl, const char *path, const char *iface);
int  bt_path_valid(const char *path);   /* /org/bluez/hciN/dev_XX_XX_XX_XX_XX_XX */
int  bt_uuid16(const char *uuid);       /* a UUID on the Bluetooth base UUID as 16 bits, -1 otherwise */
/* Whether a device should be made Trusted after op ("pair", "connect", or "authorize": our agent let it
 * use a service) ended with err (a D-Bus error name, NULL if it worked). Trusted devices reconnect by
 * themselves at boot, before anyone logs in (no agent). */
int  bt_should_trust(const char *op, const char *err, const BtDevice *d);

#endif
