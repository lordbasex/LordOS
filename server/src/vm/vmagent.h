/* vmagent.h — org.lordos.agent, Machines' own channel into a guest (docs/VIRTUALIZATION.md 6.8, 6.17, 7.1.1), and
 * the bits lord-vm needs to talk to qemu-guest-agent.
 *
 * A virtio-serial port named org.lordos.agent: no network, no port, no VNC; lordos-guest (tools/guest) holds the guest's
 * end as the person signed in at the guest's display. Frames, both ways:
 *
 *   u32 length (little endian, of the payload)  u16 kind  u16 channel  payload
 *
 * At most AG_FRAME_MAX bytes of payload; anything longer, or a stream that doesn't parse, is dropped whole (the guest
 * is untrusted: lord-vm parses this inside its own sandbox, never lord-server). Kinds a side doesn't know are skipped,
 * so either side can grow.
 *   AGK_HELLO  guest → host  "lordos-guest <version> <capability>…" (text); the host answers with its own hello
 *   AGK_CLIP   both ways     the clipboard's text, UTF-8; only in the directions the machine allows (machine.json)
 *   AGK_CLIPX  both ways     the clipboard in up to three forms at once (capability "clipx" in both hellos): parts of
 *                            u8 form (AGF_TEXT, AGF_HTML, AGF_PNG), u32 length (little endian), bytes; each form at most
 *                            once. Text and HTML are UTF-8 of at most AG_TEXT_MAX; a PNG at most AG_PNG_MAX bytes and
 *                            AG_PNG_DIM pixels a side. lord-vm checks the PNG's header and sanitizes the HTML (only
 *                            formatting tags and attributes stay, no scripts, styles that load anything, events or
 *                            links other than http, https and mailto) before anything goes on; lord-server re-encodes
 *                            the picture in lord-media's sandbox.
 * The host never asks the guest to run anything over this channel, and the guest can't ask the host for anything. */
#ifndef LORD_VM_AGENT_H
#define LORD_VM_AGENT_H

#include <stddef.h>
#include <stdint.h>

#define AG_TEXT_MAX  (1u << 20)
#define AG_PNG_MAX   (8u << 20)
#define AG_PNG_DIM   8192u
#define AG_FRAME_MAX (AG_PNG_MAX + 2 * AG_TEXT_MAX + 64)
#define AG_HEAD 8
enum { AGK_HELLO = 1, AGK_CLIP = 2, AGK_CLIPX = 3 };
enum { AGF_TEXT = 1, AGF_HTML = 2, AGF_PNG = 3 };

/* the parts of an AGK_CLIPX payload (pointers into it; NULL/0 for a form that isn't there) */
typedef struct { const uint8_t *text, *html, *png; uint32_t text_n, html_n, png_n; } AgClip;
/* 0 a well-formed payload with at least one part (lengths within the limits, no form twice), -1 otherwise. Contents
 * aren't checked here: ag_utf8_ok, ag_png_check, ag_html_sanitize */
int ag_clipx_parse(const uint8_t *p, size_t n, AgClip *c);
size_t ag_clipx_size(const AgClip *c);
void ag_clipx_write(const AgClip *c, uint8_t *out);         /* ag_clipx_size(c) bytes */
/* a PNG whose header can be believed: signature, IHDR first, 1..AG_PNG_DIM a side, a valid depth and color type. 0 ok */
int ag_png_check(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h);
/* HTML from the other side, made harmless: formatting tags and their text only (see the top of vmagent.c). Returns the
 * output's length (≤ cap - 1, NUL-terminated), or -1 if it doesn't fit. Input must be valid UTF-8. */
long ag_html_sanitize(const char *in, size_t n, char *out, size_t cap);

typedef struct { uint16_t kind, chan; const uint8_t *data; uint32_t len; } AgFrame;

/* the next frame at buf: >0 bytes it took (f filled), 0 not complete yet, -1 malformed (too long) */
int ag_frame_next(const uint8_t *buf, size_t n, AgFrame *f);
/* the header for a payload of len bytes */
void ag_frame_head(uint8_t out[AG_HEAD], uint16_t kind, uint16_t chan, uint32_t len);
/* valid UTF-8 without NUL (what a clipboard text may be). 1 yes */
int ag_utf8_ok(const uint8_t *p, size_t n);
/* base64 (qemu-guest-agent's file and exec data): encoded/decoded length, or -1 */
long ag_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
long ag_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif
