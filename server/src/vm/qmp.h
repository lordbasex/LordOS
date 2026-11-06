/* qmp.h — lord-vm's side of QEMU's control socket (QMP): one JSON object per line. QEMU is untrusted: a line is
 * at most QMP_LINE_MAX bytes and parsed by the strict reader (sec/json.c), and nothing waits on QEMU except the
 * greeting, with a timeout. Replies come back in the order the commands went out, so lord-vm keeps a queue. */
#ifndef LORD_VM_QMP_H
#define LORD_VM_QMP_H

#include "../sec/json.h"
#include <stddef.h>

#define QMP_LINE_MAX (256 * 1024)

typedef struct {
    int fd;
    char *buf;
    size_t n;
    int overflow;           /* skipping the rest of an over-long line */
    unsigned long long lines, refused;
} Qmp;

int  qmp_open(Qmp *q, int fd);
void qmp_close(Qmp *q);
/* greeting and qmp_capabilities, waiting at most timeout_ms. 0 ok */
int  qmp_handshake(Qmp *q, int timeout_ms, char *err, size_t errlen);
/* sends {"execute": cmd, "arguments": args} (args: a JSON object or NULL) with an optional descriptor. 0 sent */
int  qmp_command(Qmp *q, const char *cmd, const char *args, int fd);
/* sends a whole command as it is (development: lord-vm --dev-qmp). 0 sent */
int  qmp_raw(Qmp *q, const char *json);
/* reads what's there without waiting: 0 ok, -1 QEMU closed the socket */
int  qmp_fill(Qmp *q);
/* Takes the next complete line as JSON. 1 a message (free it with json_free), 0 none yet. A line that isn't a JSON
 * object is counted in `refused` and skipped. */
int  qmp_next(Qmp *q, JDoc *d);

/* what a parsed message is */
enum { QMP_MSG_OTHER = 0, QMP_MSG_RETURN, QMP_MSG_ERROR, QMP_MSG_EVENT, QMP_MSG_GREETING };
int  qmp_kind(const JVal *root);
/* "error": {"desc": ...} of an error reply, or "" */
const char *qmp_error_desc(const JVal *root);

#endif
