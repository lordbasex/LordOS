#include "admin.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(admin_start("root") == 0);
    assert(admin_fd() == -1);
    static const char *const actions[] = {
        "pkg-install", "pkg-update", "net-restore", "net-manage", "install-run",
        "locale-gen", "langpack", "ssh-set", "users-create", "disk-format"
    };
    for (unsigned i = 0; i < sizeof actions / sizeof *actions; i++) {
        assert(!admin_known(actions[i]));
        assert(admin_send(actions[i], "", 0) == -1);
    }
    const char *const cmd[] = { "/bin/true", NULL };
    assert(admin_run(cmd) == -1);
    puts("standalone: no privileged distribution actions");
}
