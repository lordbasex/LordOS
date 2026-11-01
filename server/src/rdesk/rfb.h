/* rfb.h — a small VNC client: RFB 3.8, no auth or VNC auth, Raw, CopyRect and Hextile. Everything read comes from the
 * network and is untrusted (tests/rdesk_test.c). */
#ifndef LORD_RDESK_RFB_H
#define LORD_RDESK_RFB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int (*rd)(void *ud, void *buf, size_t n);          /* exactly n bytes, 0 ok */
    int (*wr)(void *ud, const void *buf, size_t n);
    uint32_t *(*fb)(void *ud, int w, int h);           /* the picture for this size, NULL refuses it */
    void (*frame)(void *ud);
    void *ud;
    uint32_t *px;
    int w, h;
} Rfb;

void des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);
void rfb_challenge(const char *password, const uint8_t in[16], uint8_t out[16]);
int  rfb_handshake(Rfb *r, char *password);            /* 0 ok, 1 denied, -1 protocol or network; the password is wiped */
int  rfb_update_request(Rfb *r, int incremental);
int  rfb_message(Rfb *r);                              /* one server message: 0 ok, -1 ends */
int  rfb_key(Rfb *r, uint32_t keysym, int down);
int  rfb_pointer(Rfb *r, int x, int y, int buttons);

#endif
