/* proto.h — lord-server ↔ lord-rdesk, the helper behind the RDP and VNC apps (docs/FILES.md, "Remote desktop"). A
 * SOCK_SEQPACKET pair on fd 3. The first packet is the job as JSON; then fixed messages. The picture is a memfd lord-server
 * makes and shares: the helper draws BGRA into it and the engine shows it as a media node. */
#ifndef LORD_RDESK_PROTO_H
#define LORD_RDESK_PROTO_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum { RD_SIZE = 1, RD_FRAME, RD_STATE, RD_CERT, RD_SHM, RD_KEY, RD_PTR, RD_CERT_OK, RD_CLOSE };
enum { RS_CONNECTING = 0, RS_CONNECTED, RS_DENIED, RS_FAILED, RS_ENDED, RS_UNREACHABLE };

typedef struct {
    uint32_t type;
    int32_t a, b, c;
    char text[160];             /* RD_CERT: the fingerprint */
} RdMsg;

#define RD_MAX_W 7680
#define RD_MAX_H 4320
#define RD_POLL_MAX 256                 /* messages taken per poll: a flood from the helper doesn't hold the UI thread */

/* The shared picture, its size sealed: the helper can't shrink it under lord-server's mapping. */
static inline int rd_picture_memfd(size_t len) {
    int fd = (int)syscall(SYS_memfd_create, "lordos-rdesk", 3u /* MFD_CLOEXEC | MFD_ALLOW_SEALING */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { close(fd); return -1; }
    return fd;
}

/* Passes the picture to the helper; a full socket never blocks lord-server. 0 sent. */
static inline int rd_send_picture(int sock, int fd) {
    RdMsg r = { RD_SHM, 0, 0, 0, "" };
    struct iovec io = { &r, sizeof r };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
    memset(&cm, 0, sizeof cm);
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
    struct cmsghdr *ch = CMSG_FIRSTHDR(&mh);
    ch->cmsg_level = SOL_SOCKET; ch->cmsg_type = SCM_RIGHTS; ch->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(ch), &fd, sizeof fd);
    return sendmsg(sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof r ? 0 : -1;
}

/* One message without waiting. 1 got it, 0 nothing now or the budget is spent, -1 the helper hung up. */
static inline int rd_take(int sock, RdMsg *m, int *budget) {
    if (*budget <= 0) return 0;
    --*budget;
    ssize_t n = recv(sock, m, sizeof *m, MSG_DONTWAIT);
    return !n ? -1 : n == (ssize_t)sizeof *m ? 1 : 0;
}

#endif
