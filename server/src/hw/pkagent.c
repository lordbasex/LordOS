/* pkagent.c — see pkagent.h.
 *
 * Why it registers per process. polkit finds an agent by the subject's logind session, and a LordOS
 * session has none it can see: lord-server and its apps live under lordos.service's own cgroup tree
 * (docs/PRIVACY.md, "Root-owned app scopes"), so sd_pid_get_session and sd_pid_get_owner_uid both
 * fail for them. Moving that tree under user.slice would move the firewall, the Monitor and every
 * scope with it. polkit's other way is an agent for one process, the one pkttyagent --process uses:
 * /usr/bin/pkexec here is lord-pkexec, which asks this file to register for the process that called
 * it, waits for the answer and execs the real pkexec, with the same pid and arguments.
 *
 * The password. Keys of this computer's keyboards reach pkagent_key before anything else, so they
 * never enter JavaScript, an input method or a Wayland client. They stay in a locked page (pkfield.c)
 * and are written once to polkit-agent-helper-1's stdin, then wiped. Nothing else can type here or
 * press Enter: injected keys are dropped and JavaScript has no way to submit. Nothing from the field is
 * ever logged. polkit's helper checks the password and tells polkitd by itself. */
#define _GNU_SOURCE
#include "pkagent.h"
#include "pkfield.h"
#include "jbuf.h"
#include "sysbus.h"
#include "../keyboard.h"
#include "../sec/appid.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define POLKIT      "org.freedesktop.PolicyKit1"
#define POLKIT_PATH "/org/freedesktop/PolicyKit1/Authority"
#define POLKIT_IF   POLKIT ".Authority"
#define AGENT_IF    POLKIT ".AuthenticationAgent"
#define AGENT_PATH  "/org/lordos/PolicyKit1/AuthenticationAgent"
#define E_CANCELLED POLKIT ".Error.Cancelled"
#define HELPER      "/usr/lib/polkit-1/polkit-agent-helper-1"
#define SOCK_NAME   "lordos-pkagent.sock"
#define MAX_REQ     4                   /* waiting requests; a fifth is refused */
#define MAX_REG     64                  /* registered processes */
#define MAX_TRIES   3

typedef enum { ST_ASK, ST_CHECK } State;

typedef struct {
    int id;
    DMsg call;                          /* owned: the reply goes when it ends */
    char cookie[128], user[64];
    char action[128], message[400], program[300], command[400], as_user[80], who[128], comm[32];
    int tries;
} Req;

static PkEmit emit;
static int on, next_id = 1, keyboard_here;
static char owner[256];
static uid_t me;
static char locale_name[64];

static Req q[MAX_REQ];
static int nq;
static State st;
static int wrong;
static PkField field;
static unsigned char held[(KEY_MAX + 8) / 8];

static pid_t helper = -1, zombie[8];
static int helper_in = -1, helper_out = -1, sent;
static char line[2048];
static size_t line_n;

static int lsn = -1, conn = -1;
static unsigned conn_gen;
static struct { unsigned pid; unsigned long long start; } reg[MAX_REG];
static int nreg;

static int is_held(int c) { return c >= 0 && c <= KEY_MAX && (held[c / 8] >> (c % 8) & 1); }
static void set_held(int c, int v) {
    if (c < 0 || c > KEY_MAX) return;
    if (v) held[c / 8] |= (unsigned char)(1 << (c % 8)); else held[c / 8] &= (unsigned char)~(1 << (c % 8));
}

/* ---- what JavaScript sees: never the field, only how many characters ---- */

static void show(void) {
    HBuf b = { 0 };
    if (!nq) hb_printf(&b, "null");
    else {
        const Req *r = &q[0];
        hb_printf(&b, "{\"id\":%d,\"state\":\"%s\",\"wrong\":%s,\"count\":%zu,\"keyboard\":%s,\"action\":", r->id,
                  st == ST_CHECK ? "check" : "ask", wrong ? "true" : "false", pkf_chars(&field), keyboard_here ? "true" : "false");
        hb_str(&b, r->action);
        hb_printf(&b, ",\"message\":");   hb_str(&b, r->message);
        hb_printf(&b, ",\"program\":");   hb_str(&b, r->program);
        hb_printf(&b, ",\"command\":");   hb_str(&b, r->command);
        hb_printf(&b, ",\"asUser\":");    hb_str(&b, r->as_user);
        hb_printf(&b, ",\"who\":");       hb_str(&b, r->who);
        hb_printf(&b, ",\"comm\":");      hb_str(&b, r->comm);
        hb_printf(&b, ",\"account\":");   hb_str(&b, r->user);
        hb_printf(&b, "}");
    }
    char *j = hb_take(&b);
    if (j && emit) emit("polkit", j);
    free(j);
}

/* ---- the helper ---- */

static void helper_stop(void) {
    if (helper_in >= 0) { close(helper_in); helper_in = -1; }
    if (helper_out >= 0) { close(helper_out); helper_out = -1; }
    if (helper > 0) {
        kill(helper, SIGTERM);          /* its own user's process until it has changed; a setuid one ends on EOF */
        for (int i = 0; i < 8; i++) if (zombie[i] <= 0) { zombie[i] = helper; break; }
    }
    helper = -1;
    line_n = 0;
    sent = 0;
}

static void reap(void) {
    for (int i = 0; i < 8; i++) if (zombie[i] > 0 && waitpid(zombie[i], NULL, WNOHANG) != 0) zombie[i] = 0;
}

static int helper_start(const Req *r) {
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return -1;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return -1; }
    pid_t p = fork();
    if (p < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return -1; }
    if (p == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < 32; s++) if (s != SIGKILL && s != SIGSTOP) signal(s, SIG_DFL);
        int null = open("/dev/null", O_WRONLY);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(null, 2);
        syscall(SYS_close_range, 3U, ~0U, 0U);
        char *const argv[] = { (char *)HELPER, (char *)r->user, NULL };
        char *const envp[] = { NULL };
        execve(HELPER, argv, envp);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    helper = p;
    helper_in = in[1];
    helper_out = out[0];
    fcntl(helper_in, F_SETFL, O_NONBLOCK);
    fcntl(helper_out, F_SETFL, O_NONBLOCK);
    char c[140];
    int n = snprintf(c, sizeof c, "%s\n", r->cookie);
    if (write(helper_in, c, (size_t)n) != n) { helper_stop(); return -1; }
    return 0;
}

/* ---- ending a request ---- */

static void pop(void) {
    dbus_msg_free(&q[0].call);
    memmove(&q[0], &q[1], sizeof q[0] * (size_t)(nq - 1));
    nq--;
    wrong = 0;
    st = ST_ASK;
    pkf_wipe(&field);
    show();
}

static void finish(int ok) {
    helper_stop();
    if (ok) dbus_reply(sysbus_conn(), &q[0].call, NULL, NULL);
    else dbus_error(sysbus_conn(), &q[0].call, E_CANCELLED, "cancelled");
    pop();
}

static void failed(void) {
    helper_stop();
    pkf_wipe(&field);
    if (++q[0].tries >= MAX_TRIES) { finish(0); return; }
    st = ST_ASK;
    wrong = 1;
    show();
}

static void submit(void) {
    if (!nq || st != ST_ASK || !field.len) return;
    st = ST_CHECK;
    wrong = 0;
    if (helper_start(&q[0])) { failed(); return; }
    show();
}

static void helper_line(const char *l) {
    switch (pk_line_kind(l)) {
    case PKL_ASK_HIDDEN:
        if (sent) { failed(); return; }     /* a second question (a one-time code): can't be answered here */
        sent = 1;
        {
            /* the pipe is empty and bigger than the field: this write is whole */
            field.buf[field.len] = '\n';
            ssize_t w = write(helper_in, field.buf, field.len + 1);
            int bad = w != (ssize_t)field.len + 1;
            pkf_wipe(&field);
            if (bad) failed();
        }
        break;
    case PKL_ASK_SHOWN: failed(); break;    /* LordOS names the account; PAM has nothing to ask in clear */
    case PKL_SUCCESS: finish(1); break;
    case PKL_FAILURE: failed(); break;
    default: break;                         /* PAM's messages: not shown, a bad password has its own text */
    }
}

static void helper_read(void) {
    while (helper_out >= 0) {
        ssize_t r = read(helper_out, line + line_n, sizeof line - 1 - line_n);
        if (r == 0) { failed(); return; }   /* ended without a verdict */
        if (r < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) return; failed(); return; }
        line_n += (size_t)r;
        line[line_n] = 0;
        char *nl;
        while (helper_out >= 0 && (nl = strchr(line, '\n'))) {
            *nl = 0;
            size_t used = (size_t)(nl - line) + 1;
            char one[sizeof line];
            memcpy(one, line, used);
            memmove(line, line + used, line_n - used + 1);
            line_n -= used;
            helper_line(one);
        }
        if (line_n >= sizeof line - 1) { failed(); return; }
    }
}

/* ---- the keyboard ---- */

int pkagent_blocks_injected(int value) { return nq > 0 && value != 0; }

int pkagent_key(int code, int value) {
    if (value == 0) {
        if (!is_held(code)) return 0;       /* went down before the prompt: its owner needs the release */
        set_held(code, 0);
        KeyInfo k;
        kb_key(code, 0, &k);                /* keeps modifiers right */
        return 1;
    }
    if (!nq) return 0;
    if (value == 1) set_held(code, 1);
    KeyInfo k;
    kb_key(code, value, &k);
    if (code == KEY_ESC) { if (value == 1) pkagent_cancel(q[0].id); return 1; }
    if (st != ST_ASK) return 1;
    if (code == KEY_ENTER || code == KEY_KPENTER) { if (value == 1) submit(); return 1; }
    if (code == KEY_BACKSPACE) { pkf_back(&field); show(); return 1; }
    if (k.ctrl || k.meta || (k.alt && !k.altgr) || !k.chr[0]) return 1;
    if (pkf_add(&field, k.chr)) show();
    return 1;
}

void pkagent_set_keyboard(int present) {
    present = present != 0;
    if (present == keyboard_here) return;
    keyboard_here = present;
    if (nq) show();
}

void pkagent_cancel(int id) {
    if (!nq) return;
    if (q[0].id == id) { finish(0); return; }
    for (int i = 1; i < nq; i++) if (q[i].id == id) {
        dbus_error(sysbus_conn(), &q[i].call, E_CANCELLED, "cancelled");
        dbus_msg_free(&q[i].call);
        memmove(&q[i], &q[i + 1], sizeof q[0] * (size_t)(nq - i - 1));
        nq--;
        return;
    }
}

/* ---- polkit's calls ---- */

static void copy(char *out, size_t n, const char *in) { pk_clean(out, n, in); }

static int begin(DMsg *m) {
    if (!m->sig || strcmp(m->sig, "sssa{ss}sa(sa{sv})")) { dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return 1; }
    if (nq >= MAX_REQ) { dbus_error(sysbus_conn(), m, E_CANCELLED, "too many requests"); return 1; }
    Req r;
    memset(&r, 0, sizeof r);
    DRd d;
    dr_init(&d, m);
    const char *action = dr_str(&d, NULL), *message = dr_str(&d, NULL);
    dr_str(&d, NULL);                       /* the icon */
    copy(r.action, sizeof r.action, action);
    copy(r.message, sizeof r.message, message);
    unsigned subject = 0;
    size_t end = dr_arr(&d, 8);
    while (dr_more(&d, end)) {
        dr_struct(&d);
        const char *k = dr_str(&d, NULL), *v = dr_str(&d, NULL);
        if (d.err || !k || !v) break;
        /* pkexec is setuid and fills these; polkitd adds the pid */
        if (!strcmp(k, "program")) copy(r.program, sizeof r.program, v);
        else if (!strcmp(k, "command_line")) copy(r.command, sizeof r.command, v);
        else if (!strcmp(k, "user.display") || (!strcmp(k, "user") && !r.as_user[0])) copy(r.as_user, sizeof r.as_user, v);
        else if (!strcmp(k, "polkit.subject-pid")) subject = (unsigned)strtoul(v, NULL, 10);
    }
    const char *cookie = dr_str(&d, NULL);
    int rank = 0;                           /* the session's user, then anyone but root, who usually has no password */
    uid_t pick = 0;
    end = dr_arr(&d, 8);
    while (dr_more(&d, end)) {
        dr_struct(&d);
        const char *kind = dr_str(&d, NULL);
        size_t dend = dr_arr(&d, 8);
        long uid = -1;
        while (dr_more(&d, dend)) {
            dr_struct(&d);
            const char *k = dr_str(&d, NULL), *sig = dr_variant(&d);
            if (d.err || !k || !sig) break;
            if (!strcmp(k, "uid") && !strcmp(sig, "u")) uid = (long)dr_u32(&d);
            else dr_skip(&d, sig);
        }
        if (d.err || !kind || strcmp(kind, "unix-user") || uid < 0) continue;
        int this = (uid_t)uid == me ? 3 : uid != 0 ? 2 : 1;
        if (this > rank) { rank = this; pick = (uid_t)uid; }
    }
    struct passwd *pw = rank ? getpwuid(pick) : NULL;
    if (d.err || !cookie || strlen(cookie) >= sizeof r.cookie || !pw || strlen(pw->pw_name) >= sizeof r.user) {
        dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.InvalidArgs", "request");
        return 1;
    }
    snprintf(r.cookie, sizeof r.cookie, "%s", cookie);
    snprintf(r.user, sizeof r.user, "%s", pw->pw_name);
    if (subject) {
        char id[100];
        /* the scope is root-owned: an app can't claim another's name */
        if (appid_of_pid(subject, id, sizeof id) == 0) copy(r.who, sizeof r.who, id);
        char p[40];
        snprintf(p, sizeof p, "/proc/%u/comm", subject);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char c[32];
            ssize_t n = read(fd, c, sizeof c - 1);
            close(fd);
            if (n > 0) { c[n] = 0; copy(r.comm, sizeof r.comm, c); }
        }
    }
    r.id = next_id++;
    r.call = *m;
    m->raw = NULL;                          /* owned until the reply */
    q[nq++] = r;
    if (nq == 1) { st = ST_ASK; wrong = 0; pkf_wipe(&field); show(); }
    return 1;
}

static int cancel_auth(DMsg *m) {
    DRd d;
    dr_init(&d, m);
    const char *cookie = m->sig && !strcmp(m->sig, "s") ? dr_str(&d, NULL) : NULL;
    for (int i = 0; cookie && i < nq; i++) if (!strcmp(q[i].cookie, cookie)) { pkagent_cancel(q[i].id); break; }
    dbus_reply(sysbus_conn(), m, NULL, NULL);
    return 1;
}

static int on_call(DMsg *m) {
    if (!m->path || strcmp(m->path, AGENT_PATH) || !m->iface || strcmp(m->iface, AGENT_IF) || !m->member) return 0;
    /* only polkitd asks: anyone else on the system bus could otherwise draw a system prompt */
    if (!owner[0] || !m->sender || strcmp(m->sender, owner)) {
        dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.AccessDenied", "not polkit");
        return 1;
    }
    if (!strcmp(m->member, "BeginAuthentication")) return begin(m);
    if (!strcmp(m->member, "CancelAuthentication")) return cancel_auth(m);
    return 0;
}

static void on_owner(const char *name, const char *who) {
    (void)name;
    snprintf(owner, sizeof owner, "%s", who);
    nreg = 0;                               /* polkitd forgot them */
    while (nq) {
        if (nq == 1) helper_stop();
        dbus_error(sysbus_conn(), &q[nq - 1].call, E_CANCELLED, "polkit went away");
        dbus_msg_free(&q[nq - 1].call);
        nq--;
    }
    pkf_wipe(&field);
    show();
}

/* ---- registering a process (lord-pkexec) ---- */

static void put_subject(DBuf *b, unsigned pid, unsigned long long start) {
    dw_struct(b);
    dw_str(b, "unix-process");
    size_t mark = dw_arr_begin(b, 8);
    dw_struct(b); dw_str(b, "pid");        dw_variant(b, "u"); dw_u32(b, pid);
    dw_struct(b); dw_str(b, "start-time"); dw_variant(b, "t"); dw_u64(b, start);
    dw_struct(b); dw_str(b, "uid");        dw_variant(b, "i"); dw_i32(b, (int32_t)me);
    dw_arr_end(b, mark);
}

static unsigned long long start_of(unsigned pid) {
    char p[40], s[1024];
    snprintf(p, sizeof p, "/proc/%u/stat", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, s, sizeof s - 1);
    close(fd);
    if (n <= 0) return 0;
    s[n] = 0;
    return pk_stat_start(s);
}

static void sweep(void) {
    for (int i = 0; i < nreg; ) {
        if (start_of(reg[i].pid) == reg[i].start) { i++; continue; }
        DBuf b;
        dw_init(&b);
        put_subject(&b, reg[i].pid, reg[i].start);
        dw_str(&b, AGENT_PATH);
        sysbus_call(POLKIT, POLKIT_PATH, POLKIT_IF, "UnregisterAuthenticationAgent", "(sa{sv})s", &b, NULL, NULL);
        dw_free(&b);
        reg[i] = reg[--nreg];
    }
}

static void answer(const char *s) {
    if (conn < 0) return;
    (void)!send(conn, s, strlen(s), MSG_NOSIGNAL | MSG_DONTWAIT);
    close(conn);
    conn = -1;
}

typedef struct { unsigned pid, gen; unsigned long long start; } Pend;

static void registered(const DMsg *m, void *ctx) {
    Pend *p = ctx;
    int ok = m && m->type == DBUS_RETURN;
    if (ok && nreg < MAX_REG) { reg[nreg].pid = p->pid; reg[nreg].start = p->start; nreg++; }
    if (p->gen == conn_gen) answer(ok ? "ok" : "no");
    free(p);
}

static void conn_read(void) {
    char b[32];
    ssize_t n = recv(conn, b, sizeof b - 1, MSG_DONTWAIT);
    if (n < 0 && errno == EAGAIN) return;
    if (n <= 0) { close(conn); conn = -1; return; }
    b[n] = 0;
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cr, &l) || cr.uid != me) { answer("no"); return; }
    char *e;
    unsigned long want = strtoul(b, &e, 10);
    if (e == b || *e) { answer("no"); return; }
    /* itself or its parent, of the same user: nobody registers for a process it doesn't relate to */
    unsigned pid = (unsigned)want;
    char p[40];
    snprintf(p, sizeof p, "/proc/%u", pid);
    struct stat s;
    if (stat(p, &s) || s.st_uid != me) { answer("no"); return; }
    if (pid != (unsigned)cr.pid) {
        char sp[1024];
        snprintf(p, sizeof p, "/proc/%d/stat", cr.pid);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        ssize_t r = fd >= 0 ? read(fd, sp, sizeof sp - 1) : -1;
        if (fd >= 0) close(fd);
        if (r <= 0) { answer("no"); return; }
        sp[r] = 0;
        const char *cp = strrchr(sp, ')');
        unsigned ppid = 0;
        if (!cp || sscanf(cp + 1, " %*c %u", &ppid) != 1 || ppid != pid) { answer("no"); return; }
    }
    unsigned long long start = start_of(pid);
    if (!start) { answer("no"); return; }
    sweep();
    for (int i = 0; i < nreg; i++) if (reg[i].pid == pid && reg[i].start == start) { answer("ok"); return; }
    if (nreg >= MAX_REG) { answer("no"); return; }
    Pend *pd = malloc(sizeof *pd);
    if (!pd) { answer("no"); return; }
    *pd = (Pend){ pid, conn_gen, start };
    DBuf d;
    dw_init(&d);
    put_subject(&d, pid, start);
    dw_str(&d, locale_name);
    dw_str(&d, AGENT_PATH);
    if (!sysbus_call(POLKIT, POLKIT_PATH, POLKIT_IF, "RegisterAuthenticationAgent", "(sa{sv})ss", &d, registered, pd)) {
        free(pd);
        answer("no");
    }
    dw_free(&d);
}

static void conn_accept(void) {
    int c = accept4(lsn, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (c < 0) return;
    if (conn >= 0) close(conn);             /* one at a time: a client that hangs loses its place */
    conn = c;
    conn_gen++;
}

/* ---- start and the main loop ---- */

void pkagent_start(PkEmit e) {
    emit = e;
    me = getuid();
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!rt || !*rt || (size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/" SOCK_NAME, rt) >= sizeof a.sun_path) return;
    if (pkf_open(&field)) { fprintf(stderr, "pkagent: no locked memory, no authentication prompts\n"); return; }
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return;
    unlink(a.sun_path);
    mode_t um = umask(0177);
    int bad = bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8);
    umask(um);
    if (bad) { close(s); return; }
    lsn = s;
    const char *lang = getenv("LANG");
    snprintf(locale_name, sizeof locale_name, "%s", lang && *lang ? lang : "C");
    sysbus_on_call(on_call);
    sysbus_watch_name(POLKIT, on_owner);
    on = 1;
}

int pkagent_pollfds(struct pollfd *p, int max) {
    int n = 0;
    if (!on) return 0;
    if (n < max) p[n++] = (struct pollfd){ lsn, POLLIN, 0 };
    if (conn >= 0 && n < max) p[n++] = (struct pollfd){ conn, POLLIN, 0 };
    if (helper_out >= 0 && n < max) p[n++] = (struct pollfd){ helper_out, POLLIN, 0 };
    return n;
}

void pkagent_dispatch(const struct pollfd *p, int n) {
    for (int i = 0; i < n; i++) {
        if (!p[i].revents) continue;
        if (p[i].fd == lsn) conn_accept();
        else if (p[i].fd == conn) conn_read();
        else if (p[i].fd == helper_out) helper_read();
    }
}

void pkagent_tick(void) { reap(); }
