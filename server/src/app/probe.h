/* probe.h — System Information's sandbox probe (probe.c), run by app mode for the system package
 * `sysprobe` only, after its sandbox is applied. */
#ifndef LORD_APP_PROBE_H
#define LORD_APP_PROBE_H

#define PROBE_APP_ID "sysprobe"

/* Before the app's sandbox: forks the child that takes the decoding helpers' profile and tries what
 * must fail under it. Returns the pipe to read its result (and its pid in *child), or -1. */
int probe_helper_start(const char *data, int *child);
/* After the app's sandbox: tries what must fail, collects the helper child, sends everything to
 * lord-server (BOP_PROBE) and returns 0 if it was taken. */
int probe_main(const char *home, const char *data, int sandbox_mask, int helper_fd, int helper_pid);

#endif
