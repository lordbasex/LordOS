/* mcli.h — the `machines` command's pure parts (machines.c; tests/mcli_test.c): sizes, finding a machine by its name,
 * `machines set`'s options as lord-vm --set's, a new machine's folder name, a machine's state from run.json, the
 * catalog's {name} placeholders and JSON strings. No I/O here except where a FILE is passed in. */
#ifndef LORD_CLI_MCLI_H
#define LORD_CLI_MCLI_H

#include <stddef.h>
#include <stdio.h>

/* a size as people write it → MB: "8G", "8 GB", "1.5g", "8192M", "8192MiB", "2T"; a bare number is in bare_unit_mb
 * (1 for memory's MB, 1024 for a disk's GB). Whole MB, at least 1; -1 for anything else */
long long mcli_size_mb(const char *s, long long bare_unit_mb);

/* the machine `q` names among n names: the one whose name equals it ignoring ASCII case, else the ones it starts (ASCII
 * case ignored) — unique-prefix matching. Returns how many matched (0 none, 1 found, more: ambiguous), their indexes in
 * out (at most max) */
int mcli_resolve(const char *const *names, int n, const char *q, int *out, int max);

/* `machines set` → lord-vm --set's options. args: the person's words after NAME. out gets the lord-vm words (pointers into
 * args or into store, NULL-terminated, at most max - 1); store holds converted values. 0 ok; else -1 with the option
 * (and value) at fault in *bad_opt / *bad_val (bad_val NULL: it needs a value or isn't known) */
typedef struct { char v[24][24]; int n; } McliStore;
int mcli_set_args(int argc, char **args, char **out, int max, McliStore *store, const char **bad_opt, const char **bad_val);

/* the folder name the Machines app gives a machine of that name (js_vm.c folder_name): letters, digits, ' ', '-', '_'
 * and '.', others '-', at most 48 bytes; "Machine" when nothing is left */
void mcli_folder_name(const char *name, char *out, size_t n);

/* a machine's state from its run.json text (NULL or "" for none) and whether its lord-vm still lives (the pid in it is
 * a lord-vm process): state (lord-vm's: stopped, starting, running, paused, stopping, crashed) and reason, then view,
 * what Machines shows (machines.state.<view>: running, paused, starting, stopping, suspending, crashed, recovered,
 * suspended, stopped). *pid gets run.json's pid (0 none) */
typedef struct { char state[16], reason[24], view[16], since[24], detail[200]; long pid; int qemu_pid, active; } McliState;
long mcli_run_pid(const char *runjson, size_t len);
void mcli_state(const char *runjson, size_t len, int alive, McliState *st);

/* the catalog's text with {name} placeholders filled from kv ("name", "value", ..., NULL); a placeholder without a value
 * stays as it is */
void mcli_fill(const char *tmpl, const char *const *kv, char *out, size_t n);

/* s as a JSON string, quotes included */
void mcli_json_str(FILE *f, const char *s);

/* a byte count as people read it, "12.3 GB", "640 MB" (out: at least 16) */
void mcli_bytes(double b, char *out, size_t n);

#endif
