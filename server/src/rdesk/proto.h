/* proto.h — lord-server ↔ lord-rdesk, the helper behind the RDP and VNC apps (docs/FILES.md, "Remote desktop"). A
 * SOCK_SEQPACKET pair on fd 3. The first packet is the job as JSON; then fixed messages. The picture is a memfd lord-server
 * makes and shares: the helper draws BGRA into it and the engine shows it as a media node. */
#ifndef LORD_RDESK_PROTO_H
#define LORD_RDESK_PROTO_H

#include <stdint.h>

enum { RD_SIZE = 1, RD_FRAME, RD_STATE, RD_CERT, RD_SHM, RD_KEY, RD_PTR, RD_CERT_OK, RD_CLOSE };
enum { RS_CONNECTING = 0, RS_CONNECTED, RS_DENIED, RS_FAILED, RS_ENDED, RS_UNREACHABLE };

typedef struct {
    uint32_t type;
    int32_t a, b, c;
    char text[160];             /* RD_CERT: the fingerprint */
} RdMsg;

#define RD_MAX_W 7680
#define RD_MAX_H 4320

#endif
