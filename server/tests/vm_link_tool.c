/* vm_link_tool.c — plays lord-server's part of a machine group's link (vm/vmlink.c, lord.vm.link) for the headless tests
 * (tests/vm_group_link.sh) and development: `vm-link-tool <group folder> on|off`. Not installed. */
#include "../src/vm/vmlink.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) { fprintf(stderr, "usage: vm-link-tool <group folder> on|off\n"); return 2; }
    char err[256];
    if (vm_link_group(argv[1], !strcmp(argv[2], "on"), 5000, err, sizeof err)) { fprintf(stderr, "vm-link-tool: %s\n", err); return 1; }
    printf("ok\n");
    return 0;
}
