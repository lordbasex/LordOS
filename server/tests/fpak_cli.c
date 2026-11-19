/* fpak_cli.c — one command of the Flathub management list through the real runner (src/fpakrun.c), for tests on a VM:
 *   build/fpak-cli <remoteAdd|appstream|info|install|update|uninstall|uninstallUnused|list> [ref]
 * In a LordOS session lord-sessiond places only LordOS's own binaries, so on the VM it runs from LordOS's bin folder. */
#include "fpak.h"
#include "fpakrun.h"

#include <stdio.h>
#include <string.h>

static void line(void *ud, const char *l) { (void)ud; printf("| %s\n", l); }

int main(int argc, char **argv) {
    static const char *const names[FPAK_CMD_COUNT] = { "remoteAdd", "appstream", "info", "install", "update", "uninstall",
                                                       "uninstallUnused", "list" };
    int cmd = -1;
    for (int i = 0; argc > 1 && i < FPAK_CMD_COUNT; i++) if (!strcmp(argv[1], names[i])) cmd = i;
    if (cmd < 0 || argc > 3) { fprintf(stderr, "usage: fpak-cli <command> [ref]\n"); return 2; }
    int code = fpak_run(cmd, argc > 2 ? argv[2] : NULL, line, NULL, NULL);
    printf("fpak-cli: %s → exit %d\n", argv[1], code);
    return code != 0;
}
