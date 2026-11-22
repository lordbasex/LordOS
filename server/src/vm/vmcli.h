/* vmcli.h — the session channel of the `machines` command (server/src/cli/machines.c, docs/VIRTUALIZATION.md "From the
 * terminal"; approved by the maintainer 2026-11-21). Pure: lord-server (ui/js_vmcli.c) and the command both use it,
 * and tests/vmcli_test.c checks it.
 *
 * The channel: $XDG_RUNTIME_DIR/lordos-machines.sock, an AF_UNIX stream socket of the person's own lord-server, 0600,
 * and lord-server accepts a peer only when SO_PEERCRED says it runs as its own uid. One request per connection, one
 * line, then one answer line, then lord-server closes it. The requests are a closed set, each naming one machine folder:
 *
 *   "open <dir>\n"            bring up that machine's window (the Machines app's openMachine) — it must be running
 *   "start <dir>\n"           start it in its window, as Play does (the window's begin(): bridged network, the vGPU)
 *   "start-detached <dir>\n"  start it with no window, as "Start Without Window" does (startPrep, then startDetached)
 *   "release <dir>\n"         it was stopped from the terminal: let lord-server give back its bridged card's macvtap
 *                             and its vGPU now (lord.vm.list's unbridge_idle and gvt_idle), not at the next list
 *
 * <dir> is the machine folder's real path, which the Machines app then has to find among its own library's machines
 * (lord.vm.list(): <~/Machines>/<name>.lordvm); anything else is refused there. The parser here only checks the form:
 * an absolute path of 2 to VMCLI_DIR_MAX bytes, ending in ".lordvm", no control characters (bytes below 0x20, 0x7f),
 * no "//", no "." or ".." component, no trailing '/'; the verb exactly one of the four, one space, then the path to
 * the newline. No arguments, options, names or commands ever.
 *
 * The answer: "<code> <text>\n", code one digit (the command's exit status: VMCLI_OK, VMCLI_FAILED...), text at most
 * VMCLI_TEXT_MAX bytes of UTF-8 without control characters (the Machines app's words, already in the person's language;
 * may be empty). */
#ifndef LORD_VM_VMCLI_H
#define LORD_VM_VMCLI_H

#include <stddef.h>

#define VMCLI_SOCK      "lordos-machines.sock"
#define VMCLI_DIR_MAX   1024
#define VMCLI_LINE_MAX  (VMCLI_DIR_MAX + 32)   /* the longest request, newline included */
#define VMCLI_TEXT_MAX  900
#define VMCLI_ANSWER_MAX (VMCLI_TEXT_MAX + 4)  /* "<d> " + text + "\n" */

enum { VMCLI_OPEN = 1, VMCLI_START, VMCLI_START_DETACHED, VMCLI_RELEASE };

/* the command's exit statuses (also the answer's code) */
enum {
    VMCLI_OK = 0,
    VMCLI_FAILED = 1,       /* the operation was refused or failed */
    VMCLI_USAGE = 2,        /* wrong command line */
    VMCLI_NOT_RUNNING = 3,  /* the machine isn't running (open, stop...) */
    VMCLI_NO_MACHINE = 4,   /* no machine by that name, or more than one */
    VMCLI_NO_SESSION = 5,   /* it needs the person's LordOS session, and there's none */
};

typedef struct {
    int verb;                       /* VMCLI_OPEN... */
    char dir[VMCLI_DIR_MAX + 1];
} VmCliReq;

/* the verb's wire name ("open", "start", "start-detached", "release"); NULL for another */
const char *vmcli_verb_name(int verb);
/* one request line (with its newline, nothing after it) → 0 and *r, or -1 */
int vmcli_req_parse(const char *buf, size_t n, VmCliReq *r);
/* writes the request line for verb and dir into out (at least VMCLI_LINE_MAX + 1); its length, or 0 when the request
 * wouldn't parse */
size_t vmcli_req_format(int verb, const char *dir, char *out, size_t n);
/* the answer line for code (0..9) and text (NULL as ""): control characters become spaces, invalid UTF-8 '?', cut at
 * VMCLI_TEXT_MAX bytes on a character's boundary. Length written (out: at least VMCLI_ANSWER_MAX + 1) */
size_t vmcli_answer_format(int code, const char *text, char *out, size_t n);
/* one answer line (newline included) → 0, its code and its text, or -1 */
int vmcli_answer_parse(const char *buf, size_t n, int *code, char *text, size_t tn);

#endif
