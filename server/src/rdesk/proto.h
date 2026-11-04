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

/* RD_AUDIO_SHM: right after the job when it asks for sound here, the audio ring (media/aring.h) as an fd */
enum { RD_SIZE = 1, RD_FRAME, RD_STATE, RD_CERT, RD_SHM, RD_KEY, RD_PTR, RD_CERT_OK, RD_CLOSE, RD_AUDIO_SHM };
enum { RD_SOUND_NONE = 0, RD_SOUND_HERE, RD_SOUND_THERE };
/* RS_NOTFOUND: the name doesn't resolve (a PC name typed where its address goes), told apart from RS_UNREACHABLE, an
 * address that doesn't answer. RS_DROPPED: an RDP session lost without the server giving a reason (the network went
 * away), which the app may reconnect; a sign-out or a disconnect from the remote side is RS_ENDED. */
enum { RS_CONNECTING = 0, RS_CONNECTED, RS_DENIED, RS_FAILED, RS_ENDED, RS_UNREACHABLE, RS_NOTFOUND, RS_DROPPED, RS_LAST = RS_DROPPED };

typedef struct {
    uint32_t type;
    int32_t a, b, c;
    char text[160];             /* RD_CERT: the fingerprint */
} RdMsg;

#define RD_MAX_W 7680
#define RD_MAX_H 4320
/* what a session asks for: its size (0: the default), the folders it shares as { name, path }, an administrative
 * (console) session, the remote desktop's scale in percent (0 or 100: none), where its sound plays (RD_SOUND_*) and
 * the color depth (16, 24 or 32). RDP only, except the size. */
typedef struct { int w, h, drives; const char *drive[8][2]; int admin, scale, sound, depth; } RdView;

#define RD_POLL_MAX 256                 /* messages taken per poll: a flood from the helper doesn't hold the UI thread */

/* The shared picture, its size sealed: the helper can't shrink it under lord-server's mapping. */
static inline int rd_picture_memfd(size_t len) {
    int fd = (int)syscall(SYS_memfd_create, "lordos-rdesk", 3u /* MFD_CLOEXEC | MFD_ALLOW_SEALING */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { close(fd); return -1; }
    return fd;
}

/* Passes the picture (or the audio ring) to the helper; a full socket never blocks lord-server. 0 sent. */
static inline int rd_send_fd(int sock, uint32_t type, int fd) {
    RdMsg r = { type, 0, 0, 0, "" };
    struct iovec io = { &r, sizeof r };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
    memset(&cm, 0, sizeof cm);
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
    struct cmsghdr *ch = CMSG_FIRSTHDR(&mh);
    ch->cmsg_level = SOL_SOCKET; ch->cmsg_type = SCM_RIGHTS; ch->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(ch), &fd, sizeof fd);
    return sendmsg(sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof r ? 0 : -1;
}
static inline int rd_send_picture(int sock, int fd) { return rd_send_fd(sock, RD_SHM, fd); }

/* One message without waiting. 1 got it, 0 nothing now or the budget is spent, -1 the helper hung up. */
static inline int rd_take(int sock, RdMsg *m, int *budget) {
    if (*budget <= 0) return 0;
    --*budget;
    ssize_t n = recv(sock, m, sizeof *m, MSG_DONTWAIT);
    return !n ? -1 : n == (ssize_t)sizeof *m ? 1 : 0;
}

#endif
