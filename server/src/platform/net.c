/* Only state.c and nl.c are needed to observe the host network. VPN management
 * belongs to NetworkManager/the host; do not link LordOS's privileged drivers. */
#include <stdlib.h>
#include <string.h>
char *net_vpn_types_json(void) { return strdup("[]"); }
