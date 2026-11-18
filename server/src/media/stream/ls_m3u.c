/* ls_m3u.c — lord-stream's IPTV channel list (MK_M3U, 0.8.8.10; streamurl.h). One http(s) GET of the list (at most
 * LS_M3U_MAX bytes, bounded in time), parsed here, in the sandbox; lord-server gets only channels already checked:
 * name, group, an http(s) logo and an address stream_url_ok() accepts, as JSON arrays of at most ~48 KB each
 * (MM_RESULT v[0] = 0, v[1] = 1 while more follow), then a last one with v[1] = 0. */
#include "../helper/lm.h"
#include "../streamurl.h"

#include <stdlib.h>
#include <string.h>

#define LS_M3U_MAX (4 << 20)
#define LS_M3U_CHANNELS 5000
#define LS_CHUNK 48000

static LmBuf chunk;
static int in_chunk;

static void flush_chunk(int more) {
    lm_buf_printf(&chunk, "]");
    int64_t v[6] = { 0, more, 0, 0, 0, 0 };
    mp_send(LM_SOCK, MM_RESULT, 1, v, chunk.d, chunk.len, NULL, 0);
    lm_buf_free(&chunk);
    memset(&chunk, 0, sizeof chunk);
    in_chunk = 0;
}

static void one(const StreamEntry *e, void *ud) {
    (void)ud;
    if (!in_chunk) { lm_buf_printf(&chunk, "["); }
    else lm_buf_printf(&chunk, ",");
    lm_buf_printf(&chunk, "{\"name\":");
    lm_buf_str(&chunk, e->name);
    lm_buf_printf(&chunk, ",\"group\":");
    lm_buf_str(&chunk, e->group);
    lm_buf_printf(&chunk, ",\"logo\":");
    lm_buf_str(&chunk, e->logo);
    lm_buf_printf(&chunk, ",\"url\":");
    lm_buf_str(&chunk, e->url);
    lm_buf_printf(&chunk, "}");
    in_chunk++;
    if (chunk.len > LS_CHUNK) flush_chunk(1);
}

static void fail(const char *code) {
    int64_t v[6] = { -1, 0, 0, 0, 0, 0 };
    mp_send(LM_SOCK, MM_RESULT, 1, v, code, strlen(code), NULL, 0);
}

static int ls_interrupt(void *op) { double *dl = op; return lm_net_abort || lm_now_ms() > *dl; }

void ls_m3u(const char *params) {
    static char url[STREAM_URL_MAX + 1];
    if (mp_param(params, "url", url, sizeof url) || !stream_m3u_url_ok(url)) { fail("media.streamAddress"); return; }
    double deadline = lm_now_ms() + 30000;
    AVIOInterruptCB cb = { ls_interrupt, &deadline };
    AVDictionary *o = NULL;
    av_dict_set(&o, "protocol_whitelist", "http,https,tls,tcp,httpproxy", 0);
    av_dict_set(&o, "rw_timeout", "10000000", 0);
    av_dict_set(&o, "user_agent", "LordOS-stream/1", 0);
    lm_tls_verify(&o);                       /* an https list: the server's certificate is checked (lm_io.c) */
    AVIOContext *io = NULL;
    int r = avio_open2(&io, url, AVIO_FLAG_READ, &cb, &o);
    av_dict_free(&o);
    memset(url, 0, sizeof url);
    if (r < 0 || !io) { fail(r == AVERROR_HTTP_UNAUTHORIZED || r == AVERROR_HTTP_FORBIDDEN ? "media.streamAuth" : "media.streamCannotOpen"); return; }
    char *text = malloc(LS_M3U_MAX + 1);
    size_t len = 0;
    while (text && len < LS_M3U_MAX) {
        int n = avio_read(io, (unsigned char *)text + len, (int)(LS_M3U_MAX - len > 65536 ? 65536 : LS_M3U_MAX - len));
        if (n <= 0) break;
        len += (size_t)n;
    }
    avio_closep(&io);
    if (!text) { fail("sys.outOfMemory"); return; }
    text[len] = 0;
    int n = stream_m3u_parse(text, len, LS_M3U_CHANNELS, one, NULL);
    free(text);
    if (!n) { fail("media.streamNoChannels"); return; }
    if (!in_chunk) lm_buf_printf(&chunk, "[");
    flush_chunk(0);
}
