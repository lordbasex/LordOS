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

/* ---- exec, cp, logs, mcp and apply (docs/VIRTUALIZATION.md 6.15, 6.24) ---- */

/* `machines cp`'s side in a machine: "NAME:PATH" (NAME without '/', not empty; PATH "" means the guest's home, "~").
 * 1 and name, *path filled; 0 a path on this computer (write ./a:b for a local name with a colon) */
int mcli_cp_side(const char *arg, char *name, size_t nn, const char **path);

/* cp of a folder (6.15): a path relative to the folder, as the guest's listing gives it (hostile): components of 1 to
 * 255 bytes, never "." or "..", no leading or trailing '/', no control characters, at most MCLI_TREE_DEPTH deep and
 * MCLI_TREE_PATH_MAX bytes */
#define MCLI_TREE_DEPTH    32
#define MCLI_TREE_PATH_MAX 3072
#define MCLI_TREE_MAX      10000          /* files and folders in one copy */
int mcli_rel_path_ok(const char *p);
/* the guest's listing: entries "d PATH\0" and "f SIZE PATH\0" (find -printf). 1 an entry (e->path points into b),
 * 0 the end, -1 anything else (the whole copy is refused) */
typedef struct { int dir; unsigned long long size; const char *path; } McliTreeEnt;
int mcli_tree_next(const char *b, size_t n, size_t *off, McliTreeEnt *e);

/* n bytes as a JSON string (quotes included): every byte that isn't valid UTF-8 becomes U+FFFD, controls are escaped */
#include <stdint.h>
void mcli_json_bytes(FILE *f, const uint8_t *b, size_t n);

/* what a guest wrote, made safe for a terminal: newlines and tabs kept, other controls, C1 controls and broken UTF-8
 * each one '?'. Returns the length written (out NUL-terminated, cap ≥ 5) */
size_t mcli_term_clean(const uint8_t *b, size_t n, char *out, size_t cap);

/* a reproducible machine's file (`machines apply`, 6.24): a strict subset of TOML. An optional [machine] table first,
 * then `key = value` lines (comments with #): name, arch, memory ("8G"), disk ("64G"), network, iso, graphics,
 * clipboard (strings), cpus (1..64), agent (true/false), ports, shares (one-line lists of strings, as `machines set`'s
 * --port and --share take them). Unknown or repeated keys, anything after a value, other value forms: refused, with
 * the line (*line, 0 for the whole file) and why. The values are checked again by lord-vm. */
#define MCLI_APPLY_MAX    (64 * 1024)
#define MCLI_APPLY_STR    1100
#define MCLI_APPLY_PORTS  32
#define MCLI_APPLY_SHARES 4
typedef struct {
    char name[65], arch[24], memory[24], disk[24], network[24], graphics[24], clipboard[24], iso[1024];
    int cpus, agent;                        /* -1: not given */
    char ports[MCLI_APPLY_PORTS][48];
    char shares[MCLI_APPLY_SHARES][1100];
    int n_ports, n_shares, ports_set, shares_set;
} McliApply;
int mcli_apply_parse(const char *text, size_t n, McliApply *a, int *line, const char **why);

/* one MCP message (JSON-RPC 2.0, one per line, at most MCLI_RPC_MAX bytes): exactly the keys jsonrpc "2.0", method (1..64
 * bytes), id (an integer or a string of at most 128 bytes; absent: a notification) and params (an object or array).
 * 0 with r filled (id: its JSON text, to echo back; free with mcli_rpc_free), -1 refused */
#include "../sec/json.h"
#define MCLI_RPC_MAX (1 << 20)
typedef struct { JDoc doc; const char *method; const JVal *params; int has_id; char id[800]; } McliRpc;
int mcli_rpc_parse(const char *line, size_t n, McliRpc *r);
void mcli_rpc_free(McliRpc *r);

/* an MCP client names a machine of the library only: 1..300 bytes, no '/', no control characters, not ending .lordvm or
 * .lordgroup (never a folder path: it could point at a machine.json outside the library). 1 yes */
int mcli_mcp_name_ok(const char *q);

/* what an apply file asks that crosses this computer's boundary (an ISO to read, forwarded ports, shared folders, a
 * bridged or host-only network, the clipboard, the guest agent): their list, "; "-separated, in out; how many (each one
 * needs the person's yes on their terminal) */
int mcli_apply_risks(const McliApply *a, char *out, size_t n);

/* an apply file's ISO, resolved: a .iso or .img file directly or deeper under one of roots (NULL-terminated: the
 * person's Downloads folder, the Machines library). 1 yes */
int mcli_iso_place_ok(const char *real, const char *const *roots);

#endif
