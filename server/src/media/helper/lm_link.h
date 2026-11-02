/* lm_link.h — lord-media's end of a live link (../live.h): nothing here ever waits. */
#ifndef LORD_MEDIA_LM_LINK_H
#define LORD_MEDIA_LM_LINK_H

#include "../live.h"
#include <stddef.h>

typedef struct {
    int fd;
    uint8_t *in, *out;
    size_t in_len, in_cap, out_len;
} LmLink;

typedef void (*LmLinkFn)(const LiveHdr *h, const uint8_t *data, void *arg);

void lm_link_init(LmLink *l, int fd);
void lm_link_close(LmLink *l);
/* Reads what's there and calls fn per whole message. 0 goes on, -1 the link ended or broke the protocol. */
int  lm_link_read(LmLink *l, LmLinkFn fn, void *arg);
/* Queues one message; dropped whole when the other side is too far behind. 0 queued or sent. */
int  lm_link_send(LmLink *l, int type, int flags, int64_t pts, const void *data, size_t n);
void lm_link_flush(LmLink *l);
short lm_link_events(const LmLink *l);  /* for poll */

#endif
