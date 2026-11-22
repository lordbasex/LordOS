/* stout.h — LordOS Studio's loopback output, the pure parts (docs/CAMERA-STUDIO-PLAN.md 2.2, 8.2; phase 2).
 *
 * lord-studio writes the "LordOS Studio" V4L2 device (/dev/video50, v4l2loopback, an fd lord-server opened) in YUYV
 * at a fixed size. With no reader it writes a still card once a second with the real camera closed, so browsers keep
 * listing the device (v4l2loopback's exclusive_caps announces CAPTURE only while a writer streams). The cards are
 * drawn here from numbers, never from a file or text: the idle card (a camera glyph) and the off card (the same glyph
 * crossed out, when the person turned LordOS Studio off while an app still reads it).
 *
 * Nothing here opens anything or talks to the kernel: stcam.c does the device's part. build/studio-test checks it. */
#ifndef LORD_STUDIO_STOUT_H
#define LORD_STUDIO_STOUT_H

#include <stddef.h>
#include <stdint.h>

enum { ST_CARD_IDLE = 0, ST_CARD_OFF = 1 };

/* the output's sizes: even, 320×180 .. 1920×1080 (the plan's 640×360, 1280×720 and 1920×1080) */
#define ST_OUT_MIN_W 320
#define ST_OUT_MIN_H 180
#define ST_OUT_MAX_W 1920
#define ST_OUT_MAX_H 1080

/* w×h clamped into the range above and made even */
void st_out_size(int *w, int *h);
/* bytes of one YUYV frame of w×h */
size_t st_yuyv_size(int w, int h);
/* a card (ST_CARD_*) as BGRA w×h (stride w·4) */
void st_card_bgra(int card, uint8_t *bgra, int w, int h);
/* BGRA (w×h, any stride) → YUYV 4:2:2 (BT.601, limited range: what V4L2 consumers assume for YUYV), w even */
void st_bgra_to_yuyv(const uint8_t *bgra, int stride, int w, int h, uint8_t *yuyv);

/* v4l2loopback's client-usage event (V4L2_EVENT_PRI_CLIENT_USAGE, 0.13.0 and later; its value in v4l2loopback.c is
 * V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1): its payload is a 32-bit count of the readers streaming now. In 0.15 that
 * is 0 or 1: one reader holds the capture stream at a time (read 2026-11-22 in v0.15.0's source). Returns the count, or
 * -1 when the event is not that one. */
#define ST_EVENT_CLIENT_USAGE (0x08000000u + 0x08E00000u + 1u)
int st_client_usage(uint32_t type, const uint8_t *data, size_t len);

#endif
