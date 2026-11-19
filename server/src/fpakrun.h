/* fpakrun.h — one Flatpak management command, run in its own scope (fpakrun.c, docs/FLATPAK.md E3). */
#ifndef LORD_FPAKRUN_H
#define LORD_FPAKRUN_H
#include <sys/types.h>
#define FPAK_LINE_MAX 400
/* /usr/bin/flatpak is root's and not writable by others */
int fpak_binary_ok(void);
/* the arch Flatpak apps run on here ("x86_64"), or NULL where phase 1 offers none */
const char *fpak_native_arch(void);
/* Runs it and waits: on_line for each line of output, on_pid with the child's pid (its own process group) and 0 when it
 * ended. → its exit status (127 not started, 126 not placed in its scope, 128 + signal). */
int fpak_run(int cmd, const char *ref, void (*on_line)(void *ud, const char *line), void *ud, void (*on_pid)(void *ud, pid_t pid));
#endif
