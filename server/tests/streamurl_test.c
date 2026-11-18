/* streamurl_test.c — the addresses lord-stream may open and the M3U lists it reads (src/media/streamurl.c). */
#include "../src/media/streamurl.h"

#include <stdio.h>
#include <string.h>

static int fails = 0, oks = 0;
#define CHECK(name, cond) do { if (cond) oks++; else { fails++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

static int n_seen;
static StreamEntry seen[8];
static void keep(const StreamEntry *e, void *ud) { (void)ud; if (n_seen < 8) seen[n_seen] = *e; n_seen++; }

int main(void) {
    const char *good[] = { "rtsp://admin:22222ddddd@172.16.0.75:554/cam/realmonitor?channel=1&subtype=1", "rtsps://cam.example.org/live",
                           "rtmp://live.example.org/app/key", "srt://10.0.0.5:9000?mode=caller&latency=200", "rtp://239.0.0.1:5004",
                           "udp://@239.1.1.1:1234", "https://example.org/live/index.m3u8", "http://192.168.1.20/mjpg/video.mjpg",
                           "https://example.org/dash/manifest.mpd", "rtmpt://example.org/app", NULL };
    for (int i = 0; good[i]; i++) { char nm[160]; snprintf(nm, sizeof nm, "allowed: %.120s", good[i]); CHECK(nm, stream_url_ok(good[i])); }
    const char *bad[] = { "file:///etc/passwd", "/home/user/video.mp4", "concat:http://a|http://b", "pipe:0", "data:text/plain,x",
                          "subfile,,start,0,end,0,,:file:///etc/shadow", "crypto+file:///x", "async:http://example.org/a",
                          "http://example.org/a b", "http://example.org/a\nb", "rtsp:///nohost", "ftp://example.org/a", "HTTP://example.org/x",
                          "http://example.org/\"x", "http://example.org/|x", "https://", "", NULL };
    for (int i = 0; bad[i]; i++) { char nm[160]; snprintf(nm, sizeof nm, "refused: %.120s", bad[i]); CHECK(nm, !stream_url_ok(bad[i])); }
    CHECK("refused: NULL", !stream_url_ok(NULL));
    {
        char big[STREAM_URL_MAX + 40] = "http://example.org/";
        memset(big + 19, 'a', STREAM_URL_MAX);
        big[STREAM_URL_MAX + 19] = 0;
        CHECK("refused: too long", !stream_url_ok(big));
    }
    CHECK("m3u only over http(s)", stream_m3u_url_ok("https://iptv.example.org/list.m3u") && !stream_m3u_url_ok("rtsp://cam/x"));

    const char *m3u =
        "\xef\xbb\xbf#EXTM3U\r\n"
        "#EXTINF:-1 tvg-id=\"a\" tvg-logo=\"https://logo.example.org/a.png\" group-title=\"News\",Channel A, the news\r\n"
        "https://a.example.org/a.m3u8\r\n"
        "#EXTINF:-1 tvg-logo=\"file:///etc/passwd\" group-title=\"Local\",Cam\x01 B\n"
        "rtsp://user:pw@10.0.0.2/stream\n"
        "#EXTINF:-1,Evil\n"
        "file:///etc/passwd\n"
        "#EXTINF:-1,No address after\n"
        "#EXTVLCOPT:network-caching=1000\n"
        "udp://@239.1.1.1:1234\n"
        "  http://no-extinf.example.org/x.ts  \n";
    n_seen = 0;
    int n = stream_m3u_parse(m3u, strlen(m3u), 100, keep, NULL);
    CHECK("m3u: four channels (the file: one skipped)", n == 4 && n_seen == 4);
    CHECK("m3u: name after the last comma, group and logo", !strcmp(seen[0].name, "the news"));
    CHECK("m3u: group and https logo", !strcmp(seen[0].group, "News") && !strcmp(seen[0].logo, "https://logo.example.org/a.png"));
    CHECK("m3u: a file: logo dropped, control characters out", !strcmp(seen[1].name, "Cam B") && !seen[1].logo[0] && !strcmp(seen[1].url, "rtsp://user:pw@10.0.0.2/stream"));
    CHECK("m3u: an option line between keeps the name", !strcmp(seen[2].name, "No address after") && !strcmp(seen[2].url, "udp://@239.1.1.1:1234"));
    CHECK("m3u: no #EXTINF: named by its address, spaces trimmed", !strcmp(seen[3].url, "http://no-extinf.example.org/x.ts") && seen[3].name[0]);
    n_seen = 0;
    CHECK("m3u: the maximum holds", stream_m3u_parse(m3u, strlen(m3u), 2, keep, NULL) == 2 && n_seen == 2);
    n_seen = 0;
    CHECK("m3u: not a list", stream_m3u_parse("<html>nope</html>", 17, 10, keep, NULL) == 0);
    {
        /* a name cut inside a UTF-8 character keeps whole characters only */
        char line[400] = "#EXTINF:-1,";
        for (int i = 0; i < 70; i++) strcat(line, "\xc3\xb1");
        strcat(line, "\nhttp://x.example.org/a\n");
        n_seen = 0;
        stream_m3u_parse(line, strlen(line), 5, keep, NULL);
        size_t L = strlen(seen[0].name);
        CHECK("m3u: a long name cut on a whole character", n_seen == 1 && L <= STREAM_NAME_MAX && L % 2 == 0);
    }
    printf(fails ? "streamurl-test: %d FAILED\n" : "streamurl-test: %d ok\n", fails ? fails : oks);
    return fails ? 1 : 0;
}
