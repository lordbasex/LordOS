/* vmnet.h — the pure parts of Machines' bridged network (docs/VIRTUALIZATION.md 6.5): what a card of this computer is,
 * the admin actions' arguments and the macvtap's alias. No netlink and no root here, so the tests and lord-server share
 * them with the admin helper (vmnet_admin.c). */
#ifndef LORD_VM_VMNET_H
#define LORD_VM_VMNET_H

#include <stddef.h>
#include <sys/types.h>

/* what a network card is, read from sysfs (<sys>/<name>/...; sys is "/sys/class/net", or a folder made by a test) */
enum {
    VMNET_WIRED = 0,     /* a physical Ethernet card nothing else holds: a macvtap can go on it */
    VMNET_MISSING,       /* no such card */
    VMNET_WIFI,          /* a wireless card: an access point accepts one address per station, so a macvtap's frames
                          * with the guest's own address never get through */
    VMNET_VIRTUAL,       /* no device behind it (a bridge, a VPN, a macvtap, loopback...) */
    VMNET_NOT_ETHER,     /* another link type */
    VMNET_ENSLAVED,      /* part of a bridge or a bond: its frames are already taken */
};
int vmnet_card(const char *sys, const char *name);
/* the reason's code for the UI (errors.json): "vm.bridgeNoCard", "vm.bridgeWifi"... ("" for VMNET_WIRED) */
const char *vmnet_card_error(int kind);

/* vm-net-bridge's argument, "<machine id> <card> <mac>": one space between, a lowercase UUID, a card's name
 * (vm_ifname_valid), a unicast MAC (vm_mac_valid). 0 ok */
int vmnet_bridge_arg(const char *arg, size_t len, char id[37], char card[16], char mac[18]);
/* vm-net-unbridge's argument, "<machine id>". 0 ok */
int vmnet_unbridge_arg(const char *arg, size_t len, char id[37]);

/* the alias the helper writes on the macvtaps it makes (only root can set an alias): "lordos-vm <uid> <machine id>" */
void vmnet_alias(char *out, size_t n, uid_t uid, const char *id);
/* whether an alias is one of ours: 1 and the uid (and id, 37 bytes, when wanted), else 0 */
int vmnet_alias_parse(const char *alias, uid_t *uid, char *id);

/* the wired cards for Configure → Network: a JSON array of { "name", "kind": "wired"|"wifi", "up": bool } of every
 * physical card, at most 16; malloc'd text, NULL on out of memory */
char *vmnet_cards_json(const char *sys);

#endif
