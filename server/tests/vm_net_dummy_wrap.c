/* vm_net_dummy_wrap.c — test only (build/vm-net-cli-dummy, tests/vm_bridge_dummy.sh): vmnet_card() wrapped at link time
 * (-Wl,--wrap=vmnet_card) so a scratch dummy interface named "lordtest<N>" counts as a wired card. Everything else
 * vm-net-bridge does (the macvtap, its alias, IPv6 kept off, /dev/tapN to the user 0600, vm-net-unbridge's checks) is the
 * admin helper's own code, unchanged. The real check refuses a dummy (it has no device behind it): the script shows that
 * with build/vm-net-cli first. Never installed; the real helper never links this. */
#include "vm/vmnet.h"

#include <string.h>

int __real_vmnet_card(const char *sys, const char *name);

int __wrap_vmnet_card(const char *sys, const char *name) {
    int k = __real_vmnet_card(sys, name);
    if (k == VMNET_VIRTUAL && name && !strncmp(name, "lordtest", 8)) return VMNET_WIRED;
    return k;
}
