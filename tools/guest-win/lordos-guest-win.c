/* lordos-guest-win — LordOS guest tools for Windows guests (docs/VIRTUALIZATION.md 4.3.1, 6.8, 6.17, 7.1.1, 7.2).
 *
 * One small program, Win32 only (no runtime DLLs; built with mingw-w64, libgcc and winpthread linked in). Machines
 * installs it through qemu-guest-agent into %ProgramFiles%\LordOS and starts it in every interactive session
 * (HKLM\...\Run); it runs as the person signed in, never as a service or as SYSTEM.
 *
 * It holds the guest's end of the virtio-serial port org.lordos.agent (\\.\Global\org.lordos.agent, the vioserial
 * driver) and speaks the frames of server/src/vm/vmagent.h, as tools/guest/lordos-guest.c does on Linux:
 *   - the clipboard both ways (AGK_CLIP, AGK_CLIPX): text (CF_UNICODETEXT ↔ UTF-8, CRLF ↔ LF), rich text ("HTML
 *     Format" ↔ HTML) and pictures (CF_DIB / the "PNG" format ↔ PNG, through the system's own WIC codecs). lord-vm
 *     decides which way it may go (machine.json "clipboard") and checks what this side sends. The host's clipboard
 *     comes on demand (AGK_CLIPOFFER, "clipreq"): an offer puts its forms on the clipboard without data (delayed
 *     rendering: SetClipboardData(format, NULL)); only when a program here pastes (WM_RENDERFORMAT) does this side ask
 *     the host for that form (AGK_CLIPREQ) and wait for the answer (AGK_CLIPDATA, AG_CLIP_WAIT_MS, AG_ASK_WAIT_MS when
 *     the person is asked). The host answers only while the person uses the machine; anything else renders nothing.
 *     An offer is marked so that clipboard history and cloud clipboard skip it. Leaving (WM_RENDERALLFORMATS) empties
 *     the clipboard instead of fetching what nobody pasted;
 *   - the display's size (AGK_DISPLAY): the size the machine's window wants, set on the shown display. The VirtIO
 *     GPU driver (viogpudo) keeps one "custom" mode that follows the host's size (QEMU's ui_info, which lord-vmdisplay
 *     sends) but nothing in Windows applies it: this side switches to it, a listed mode directly, else through the
 *     display configuration (SetDisplayConfig with SDC_ALLOW_CHANGES, as virtio-win's viogpuap does), which makes
 *     Windows ask the driver again (on a driver from 2026 or later it also asks the driver for the size itself,
 *     VIOGPU_SET_CUSTOM_RESOLUTION), and falls back to the largest listed mode that fits;
 *   - apps mode (LAG_*): the list of the session's windows, their icons, focus / close / minimize / restore / move
 *     of a listed window, and starting a program from the Start menu's own list (shell:AppsFolder) only by a token
 *     this side gave;
 *   - opening a file the person sent from Files' "Open with" (AGK_OPEN, docs/VIRTUALIZATION.md 7.3): the host names a
 *     token, never a path. The drop's copy step, run as this same person, wrote where it put the file under that token
 *     (%LOCALAPPDATA%\LordOS\open\<token>); this side reads that record once (not a link, at most AG_OPEN_RECORD_MS
 *     old), removes it, checks that the file is a regular file (not a link or reparse point) of the size and SHA-256
 *     the host measured, and opens it with the system's default handler (ShellExecuteEx "open", "openas" when nothing
 *     is associated): a program runs, as the person, like a double click in Explorer. It answers AGK_OPENED;
 *   - files dropped on an apps mode window (AGK_PLACE, 7.2.1): each checked the same way; once a drop's batch is all
 *     there they go to the window they were dropped on, and nowhere else: moved into the folder an Explorer window shows,
 *     WM_DROPFILES to a window that takes dropped files, or the clipboard (CF_HDROP) for any other, to paste;
 *   - the machine's performance profile (AGK_PERF, 7.1.4), only while the host holds the port and only what the person
 *     may change: the High performance power plan, visual effects for the session, transparency; put back after.
 * It never runs anything the host names, and validates every frame (vmagent.c's parsers) before acting on it.
 *
 * The port can be held by one process at a time: the helper of the session at the computer's console takes it and
 * lets it go when another session becomes the console one (switch user, Remote Desktop), or when the person logs off.
 *
 *   lordos-guest-win.exe [--port PATH] [--log PATH] [--version] */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <appmodel.h>
#include <wincodec.h>
#include <wtsapi32.h>
#include <bcrypt.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <oleidl.h>
#include <powrprof.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../server/src/vm/vmagent.h"

/* w5 (2026-11-21): "pointer": lord-vm may set the display driver's hardware pointer (viogpudo's HWCursor) through
 * qemu-guest-agent once these tools run (docs/VIRTUALIZATION.md 7.1.2); this program itself doesn't touch it */
/* w6 (2026-11-21): "gvt": lord-vm may install Intel's driver for a GVT-g vGPU, block its updates and turn hibernation
 * off through qemu-guest-agent once these tools run (server/src/vm/vmgvtwin.h); this program itself doesn't touch them */
/* w7 (2026-11-22): the display the machine's picture shows, when Windows has more than one (a GVT-g vGPU's second
 * output): the size, apps mode's coordinates and moves follow it, windows on another display come onto it, and the
 * displays go to the host's log (AGK_DISPLAYS) */
/* w8 (2026-11-22): the shown display is one on a PCI adapter whenever there is one (w7 took a basic display that had
 * the asked size), is made Windows' primary, and windows come onto it as the tools start too; a window that runs as
 * administrator is said so (LAGW_ELEVATED, when the host's hello says "elevated") and an action the system refuses is
 * answered (LAG_WINREFUSED) */
/* w9 (2026-11-22): "extra": turns off, when the host asks (AGK_EXTRA), a display that isn't on a PCI adapter next to
 * the shown one (and back on; by itself too when no PCI display is left), saying "extraoff" while it keeps one off */
/* w10 (2026-11-22): "tablet": how Windows maps the absolute tablet with several displays, read from the tablet's own raw
 * input (MOUSE_VIRTUAL_DESKTOP or not), goes to the host (AGK_TABLET), which aims clicks with it; the shown display is
 * made primary again whenever a change of mode (or Windows) leaves another one primary, not once per set of displays;
 * the display configuration is asked twice at most per size, not every 250 ms; a size Windows changes by itself right
 * after (a monitor that reappears with a new EDID) is asked again, twice at most; apps mode's list follows a change of
 * the displays at once (the shown display's corner looked up for every list) */
/* w11 (2026-11-23): "perf": the machine's performance profile (AGK_PERF, docs/VIRTUALIZATION.md 7.1.4), as the person:
 * the High performance power plan, visual effects and transparency at their minimum while the host holds the port, and
 * everything put back after (AGK_PERFSTATE says what was done) */
/* w12 (2026-11-24): "shell": the agent's shell (AGK_SHELL_*, docs/VIRTUALIZATION.md 6.18): PowerShell in a pseudo
 * console, as the person and never elevated, with flow control both ways; "links notify": a program of the person's
 * (`lordos-guest-win.exe --open-on-host URL`, `--notify-host TITLE [BODY]`, by WM_COPYDATA to this session's running
 * tools) hands over a link or a notification for the host, passed on only while the host's hello says it takes them
 * (6.12) */
#define VERSION "w12"
#define HELLO "lordos-guest " VERSION " clipboard clipx clipreq display windows open place pointer gvt extra tablet perf shell links notify"

enum { WM_APP_FRAME = WM_APP + 1, WM_APP_LOST, WM_APP_OPENED, WM_APP_SHDATA, WM_APP_SHEOF, WM_APP_SHEXIT };
enum { T_PORT = 1, T_CLIP, T_DISP, T_APPS, T_MONS, T_TABLET };

static HWND win;
static wchar_t port_path[260] = L"\\\\.\\Global\\org.lordos.agent";
static FILE *logf;
static DWORD my_session;

static void lg(const char *fmt, ...) {
    if (!logf) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(logf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

static uint64_t hash(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h ^ n;
}

/* ---- a growable byte buffer for the frames this side writes ---- */
typedef struct { uint8_t *p; size_t n, cap; int oom; } Buf;
static void bput(Buf *b, const void *d, size_t n) {
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *q = realloc(b->p, c);
        if (!q) { b->oom = 1; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b16(Buf *b, uint16_t v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; bput(b, x, 2); }
static void b32(Buf *b, uint32_t v) { uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; bput(b, x, 4); }
/* a u16-length UTF-8 string, cut on a character boundary at max bytes */
static void bstr(Buf *b, const char *s, size_t max) {
    size_t n = strlen(s);
    if (n > max) { n = max; while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80) n--; }
    b16(b, (uint16_t)n);
    bput(b, s, n);
}

/* UTF-16 → UTF-8 (malloc'd, NUL-terminated); n = -1 for a NUL-terminated input */
static char *to_utf8(const wchar_t *w, int n, int *out_n) {
    int k = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    if (k <= 0) { char *e = calloc(1, 1); if (out_n) *out_n = 0; return e; }
    char *s = malloc((size_t)k + 1);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, n, s, k, NULL, NULL);
    s[k] = 0;
    int l = n < 0 ? k - 1 : k;
    if (out_n) *out_n = l;
    return s;
}
static wchar_t *to_utf16(const uint8_t *s, size_t n) {
    int k = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s, (int)n, NULL, 0);
    if (k < 0) return NULL;
    wchar_t *w = malloc(((size_t)k + 1) * sizeof *w);
    if (!w) return NULL;
    if (k) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s, (int)n, w, k);
    w[k] = 0;
    return w;
}

/* ---- the port ---- */
static HANDLE port = INVALID_HANDLE_VALUE, reader, stop_ev, wr_ev;
static volatile LONG reader_gen;
static int host_hello, host_clipx, host_elev;      /* host_elev: the host's hello says "elevated" (LAGW_ELEVATED) */
static int host_links, host_notify;                 /* the host's hello says "links", "notify": the machine takes them */

/* the host's answers to this side's clipboard requests (AGK_CLIPDATA): handed over by the reader thread, since the
 * window's thread waits for them inside WM_RENDERFORMAT */
static CRITICAL_SECTION ans_cs;
static HANDLE ans_ev;
static struct { uint32_t gen; uint8_t form, status; uint8_t *data; uint32_t n; int set; } ans;
static void ans_take(const uint8_t *p, uint32_t n) {
    uint32_t gen, dn;
    uint8_t form, status;
    const uint8_t *d;
    if (ag_clipdata_parse(p, n, &gen, &form, &status, &d, &dn)) return;
    uint8_t *copy = dn ? malloc(dn) : NULL;
    if (dn && !copy) status = AGD_REFUSED, dn = 0;
    if (copy) memcpy(copy, d, dn);
    EnterCriticalSection(&ans_cs);
    free(ans.data);
    ans.gen = gen; ans.form = form; ans.status = status; ans.data = copy; ans.n = dn; ans.set = 1;
    LeaveCriticalSection(&ans_cs);
    SetEvent(ans_ev);
}

typedef struct { uint16_t kind; uint32_t len; uint8_t data[]; } Frame;

static DWORD WINAPI reader_main(LPVOID arg) {
    HANDLE h = (HANDLE)arg;
    LONG gen = reader_gen;
    static uint8_t buf[AG_HEAD + AG_FRAME_MAX];
    size_t have = 0;
    DWORD err = 0;
    int polled = 0;
    OVERLAPPED ov;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (;;) {
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        DWORD want = (DWORD)(sizeof buf - have) < 65536 ? (DWORD)(sizeof buf - have) : 65536;
        if (!ReadFile(h, buf + have, want, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) { err = GetLastError(); break; }
        HANDLE w[2] = { ev, stop_ev };
        DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0) { CancelIoEx(h, &ov); GetOverlappedResult(h, &ov, &got, TRUE); CloseHandle(ev); return 0; }
        if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
            err = GetLastError();
            /* vioserial refuses to keep a read waiting while it believes the host's end is closed (it does after
             * the port's device was restarted, as the install does, until the machine starts again), but hands over
             * what has arrived: then this side asks every 30 ms instead */
            if (err == ERROR_OPERATION_ABORTED && WaitForSingleObject(stop_ev, 30) == WAIT_TIMEOUT) { if (!polled++) lg("port: reading by asking every 30 ms"); continue; }
            break;
        }
        if (!got) { Sleep(50); continue; }
        have += got;
        for (;;) {
            AgFrame f;
            int used = ag_frame_next(buf, have, &f);
            if (used < 0 || (!used && have == sizeof buf)) { have = 0; break; }
            if (!used) break;
            Frame *fr = f.kind == AGK_CLIPDATA ? NULL : malloc(sizeof *fr + f.len);
            if (f.kind == AGK_CLIPDATA) ans_take(f.data, f.len);
            if (fr) {
                fr->kind = f.kind; fr->len = f.len;
                memcpy(fr->data, f.data, f.len);
                if (!PostMessageW(win, WM_APP_FRAME, (WPARAM)gen, (LPARAM)fr)) free(fr);
            }
            memmove(buf, buf + used, have - (size_t)used);
            have -= (size_t)used;
        }
    }
    CloseHandle(ev);
    PostMessageW(win, WM_APP_LOST, (WPARAM)gen, (LPARAM)err);
    return 0;
}

static void apps_stop(void);
static void shell_end(void);
static void perf_restore(const char *why, int say);
static int mons_sent;               /* the displays went to the host (AGK_DISPLAYS) since the port opened */
static int tablet_mode, tablet_sent; /* how Windows maps the tablet (AG_TABLET_*, 0 not seen yet); said to the host */
static int apps_on, apps_dirty, apps_force;   /* apps mode (below); apps_force: the next list goes even if the same */
static void port_close(const char *why) {
    if (port == INVALID_HANDLE_VALUE) return;
    lg("port closed: %s", why);
    SetEvent(stop_ev);
    if (reader) { WaitForSingleObject(reader, 5000); CloseHandle(reader); reader = NULL; }
    CloseHandle(port);
    port = INVALID_HANDLE_VALUE;
    InterlockedIncrement(&reader_gen);
    host_hello = host_clipx = host_elev = host_links = host_notify = 0;
    shell_end();                        /* the host is gone: so is its shell */
    apps_stop();
    mons_sent = 0;
    tablet_sent = 0;
    SetEvent(ans_ev);                   /* a paste waiting for the host stops waiting */
    perf_restore(why, 0);               /* the performance profile lasts only while the host holds the port */
}

static int send_frame(uint16_t kind, const uint8_t *p, uint32_t n);
static int extra_is_off(void);
/* the hello, with "extraoff" while this side keeps an extra display off */
static void say_hello(void) {
    char h[sizeof HELLO + 16];
    int n = snprintf(h, sizeof h, "%s%s", HELLO, extra_is_off() ? " extraoff" : "");
    send_frame(AGK_HELLO, (const uint8_t *)h, (uint32_t)n);
}

static int send_frame(uint16_t kind, const uint8_t *p, uint32_t n) {
    if (port == INVALID_HANDLE_VALUE) return -1;
    uint8_t *b = malloc(AG_HEAD + (size_t)n);
    if (!b) return -1;
    ag_frame_head(b, kind, 0, n);
    if (n) memcpy(b + AG_HEAD, p, n);
    size_t off = 0, total = AG_HEAD + (size_t)n;
    int ok = 1;
    while (off < total) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = wr_ev;
        ResetEvent(wr_ev);
        DWORD w = 0, chunk = total - off > 65536 ? 65536 : (DWORD)(total - off);
        if (!WriteFile(port, b + off, chunk, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) { ok = 0; break; }
        if (WaitForSingleObject(wr_ev, 10000) != WAIT_OBJECT_0) { CancelIoEx(port, &ov); GetOverlappedResult(port, &ov, &w, TRUE); ok = 0; break; }
        if (!GetOverlappedResult(port, &ov, &w, FALSE) || !w) { ok = 0; break; }
        off += w;
    }
    free(b);
    if (!ok) { port_close("a write failed"); return -1; }
    return 0;
}

static int console_session(void) { return WTSGetActiveConsoleSessionId() == my_session; }

static void port_try_open(void) {
    if (port != INVALID_HANDLE_VALUE || !console_session()) return;
    HANDLE h = CreateFileW(port_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        static DWORD last_err;
        DWORD e = GetLastError();
        if (e != last_err) { lg("can't open the port yet (error %lu)", e); last_err = e; }
        return;
    }
    port = h;
    ResetEvent(stop_ev);
    reader = CreateThread(NULL, 0, reader_main, h, 0, NULL);
    lg("port open");
    say_hello();
}

/* ---- the clipboard ---- */
static uint64_t clip_last;          /* the clipboard's main form last sent or set, so it doesn't go back and forth */
static UINT cf_html, cf_png;

static int open_clip(void) {
    for (int i = 0; i < 20; i++) { if (OpenClipboard(win)) return 1; Sleep(25); }
    return 0;
}

/* WIC, the system's own image codecs */
static IWICImagingFactory *wic(void) {
    static IWICImagingFactory *f;
    if (!f && FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) f = NULL;
    return f;
}

/* 32-bit B, G, R, A pixels, rows top first → PNG (malloc'd) */
static uint8_t *png_encode(const uint8_t *px, UINT w, UINT h, size_t *out_n) {
    IWICImagingFactory *f = wic();
    if (!f) return NULL;
    IStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    uint8_t *out = NULL;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &st))) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, NULL)) && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt)) && IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, h, w * 4, w * h * 4, (BYTE *)px)) &&
            SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
            HGLOBAL g = NULL;
            STATSTG ss;
            if (SUCCEEDED(IStream_Stat(st, &ss, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(st, &g)) && ss.cbSize.QuadPart <= AG_PNG_MAX) {
                size_t n = (size_t)ss.cbSize.QuadPart;
                void *src = GlobalLock(g);
                if (src && (out = malloc(n))) { memcpy(out, src, n); *out_n = n; }
                if (src) GlobalUnlock(g);
            }
        }
    }
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    IStream_Release(st);
    return out;
}

/* PNG → 32-bit B, G, R, A pixels, rows top first (malloc'd) */
static uint8_t *png_decode(const uint8_t *p, size_t n, UINT *w, UINT *h) {
    IWICImagingFactory *f = wic();
    if (!f) return NULL;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapSource *conv = NULL;
    uint8_t *px = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &st)) && SUCCEEDED(IWICStream_InitializeFromMemory(st, (BYTE *)p, (DWORD)n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)fr, &conv)) &&
        SUCCEEDED(IWICBitmapSource_GetSize(conv, w, h)) && *w && *h && *w <= AG_PNG_DIM && *h <= AG_PNG_DIM &&
        (uint64_t)*w * *h * 4 <= (256u << 20) && (px = malloc((size_t)*w * *h * 4))) {
        if (FAILED(IWICBitmapSource_CopyPixels(conv, NULL, *w * 4, *w * *h * 4, px))) { free(px); px = NULL; }
    }
    if (conv) IWICBitmapSource_Release(conv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return px;
}

/* the clipboard's picture as PNG: the "PNG" format when an app put one there, else the bitmap Windows makes of
 * CF_DIB / CF_BITMAP (without alpha) */
static uint8_t *clip_png(size_t *out_n) {
    if (cf_png && IsClipboardFormatAvailable(cf_png)) {
        HANDLE g = GetClipboardData(cf_png);
        size_t n = g ? GlobalSize(g) : 0;
        const uint8_t *src = g ? GlobalLock(g) : NULL;
        uint8_t *out = NULL;
        if (src && n && n <= AG_PNG_MAX && !ag_png_check(src, n, NULL, NULL) && (out = malloc(n))) { memcpy(out, src, n); *out_n = n; }
        if (src) GlobalUnlock(g);
        if (out) return out;
    }
    if (!IsClipboardFormatAvailable(CF_BITMAP)) return NULL;
    HBITMAP hb = GetClipboardData(CF_BITMAP);
    BITMAP bm;
    if (!hb || !GetObjectW(hb, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > (LONG)AG_PNG_DIM || bm.bmHeight > (LONG)AG_PNG_DIM) return NULL;
    UINT w = (UINT)bm.bmWidth, h = (UINT)bm.bmHeight;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w; bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    int rows = GetDIBits(dc, hb, 0, h, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    uint8_t *out = NULL;
    if (rows == (int)h) {
        for (size_t i = 0; i < (size_t)w * h; i++) px[i * 4 + 3] = 0xff;
        out = png_encode(px, w, h, out_n);
    }
    free(px);
    return out;
}

/* "HTML Format": the fragment between StartFragment and EndFragment (offsets in its own header), checked */
static uint8_t *clip_html(size_t *out_n) {
    if (!cf_html || !IsClipboardFormatAvailable(cf_html)) return NULL;
    HANDLE g = GetClipboardData(cf_html);
    size_t n = g ? GlobalSize(g) : 0;
    const char *s = g ? GlobalLock(g) : NULL;
    uint8_t *out = NULL;
    if (s && n > 0 && n < 64u << 20) {
        size_t head = n < 1024 ? n : 1024;
        char hd[1025];
        memcpy(hd, s, head);
        hd[head] = 0;
        const char *a = strstr(hd, "StartFragment:"), *b = strstr(hd, "EndFragment:");
        long sa = a ? atol(a + 14) : -1, sb = b ? atol(b + 12) : -1;
        while (sb > sa && sb > 0 && (size_t)sb <= n && !s[sb - 1]) sb--;
        if (sa >= 0 && sb > sa && (size_t)sb <= n && (size_t)(sb - sa) <= AG_TEXT_MAX && ag_utf8_ok((const uint8_t *)s + sa, (size_t)(sb - sa)) &&
            (out = malloc((size_t)(sb - sa)))) { memcpy(out, s + sa, (size_t)(sb - sa)); *out_n = (size_t)(sb - sa); }
    }
    if (s) GlobalUnlock(g);
    return out;
}

/* CF_UNICODETEXT as UTF-8 with LF line ends */
static uint8_t *clip_text(size_t *out_n) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return NULL;
    HANDLE g = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = g ? GlobalLock(g) : NULL;
    uint8_t *out = NULL;
    if (w) {
        size_t cap = GlobalSize(g) / sizeof(wchar_t), l = 0;
        while (l < cap && w[l]) l++;
        int k = 0;
        char *u = l ? to_utf8(w, (int)l, &k) : NULL;
        if (u) {
            int o = 0;
            for (int i = 0; i < k; i++) if (!(u[i] == '\r' && i + 1 < k && u[i + 1] == '\n')) u[o++] = u[i];
            if (o > 0 && (size_t)o <= AG_TEXT_MAX && ag_utf8_ok((uint8_t *)u, (size_t)o)) { out = (uint8_t *)u; *out_n = (size_t)o; }
            else free(u);
        }
        GlobalUnlock(g);
    }
    return out;
}

/* A password manager marks a secret it copies so that clipboard monitors and history skip it (KeePass, 1Password,
 * Bitwarden): "ExcludeClipboardContentFromMonitorProcessing", "Clipboard Viewer Ignore", or "CanIncludeInClipboardHistory"
 * holding a DWORD 0. Such a copy never leaves the guest, so it can't reach the host's clipboard history or outlive the
 * manager's own clearing. Called with the clipboard open. */
static int clip_secret(void) {
    static UINT ex, ign, hist;
    if (!ex) ex = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (!ign) ign = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    if (!hist) hist = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if ((ex && IsClipboardFormatAvailable(ex)) || (ign && IsClipboardFormatAvailable(ign))) return 1;
    if (hist && IsClipboardFormatAvailable(hist)) {
        HANDLE g = GetClipboardData(hist);
        const DWORD *d = g && GlobalSize(g) >= sizeof(DWORD) ? GlobalLock(g) : NULL;
        int no = d && *d == 0;
        if (d) GlobalUnlock(g);
        if (no) return 1;
    }
    return 0;
}

static void clip_send(void) {
    if (!host_hello || GetClipboardOwner() == win || !open_clip()) return;
    if (clip_secret()) { CloseClipboard(); lg("clipboard: a password manager's secret stays in the machine"); return; }
    size_t tn = 0, hn = 0, pn = 0;
    uint8_t *png = host_clipx ? clip_png(&pn) : NULL;
    uint8_t *html = host_clipx && !png ? clip_html(&hn) : NULL;
    uint8_t *text = png ? NULL : clip_text(&tn);
    CloseClipboard();
    uint64_t h = png ? hash(png, pn) : html ? hash(html, hn) : text ? hash(text, tn) : 0;
    if (h && h != clip_last) {
        clip_last = h;
        if (png || html) {
            AgClip c;
            memset(&c, 0, sizeof c);
            if (png) { c.png = png; c.png_n = (uint32_t)pn; }
            if (html) { c.html = html; c.html_n = (uint32_t)hn; }
            if (text) { c.text = text; c.text_n = (uint32_t)tn; }
            uint8_t *o = malloc(ag_clipx_size(&c));
            if (o) { ag_clipx_write(&c, o); send_frame(AGK_CLIPX, o, (uint32_t)ag_clipx_size(&c)); free(o); }
            lg("clipboard to the host:%s%s%s", png ? " picture" : "", html ? " html" : "", text ? " text" : "");
        } else { send_frame(AGK_CLIP, text, (uint32_t)tn); lg("clipboard to the host: text (%zu bytes)", tn); }
    }
    free(png); free(html); free(text);
}

static HGLOBAL gdup(const void *p, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    void *d = g ? GlobalLock(g) : NULL;
    if (!d) { if (g) GlobalFree(g); return NULL; }
    memcpy(d, p, n);
    GlobalUnlock(g);
    return g;
}
static void set_fmt(UINT f, HGLOBAL g) { if (g && !SetClipboardData(f, g)) GlobalFree(g); }

/* the clipboard's forms made from what the host gave (each a new HGLOBAL, or NULL) */
static HGLOBAL mk_text(const uint8_t *p, uint32_t n) {
    /* LF → CRLF, UTF-16 */
    uint8_t *t = malloc((size_t)n * 2 + 1);
    size_t o = 0;
    for (uint32_t i = 0; t && i < n; i++) { if (p[i] == '\n' && (!i || p[i - 1] != '\r')) t[o++] = '\r'; t[o++] = p[i]; }
    wchar_t *w = t ? to_utf16(t, o) : NULL;
    HGLOBAL g = w ? gdup(w, (wcslen(w) + 1) * sizeof *w) : NULL;
    free(w); free(t);
    return g;
}
static HGLOBAL mk_html(const uint8_t *p, uint32_t n) {
    static const char pre[] = "<html><body>\r\n<!--StartFragment-->", post[] = "<!--EndFragment-->\r\n</body></html>";
    char hd[200];
    int hl = snprintf(hd, sizeof hd, "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n", 0, 0, 0, 0);
    size_t total = (size_t)hl + sizeof pre - 1 + n + sizeof post - 1;
    char *b = malloc(total + 1);
    if (!b) return NULL;
    int sf = hl + (int)sizeof pre - 1, ef = sf + (int)n;
    snprintf(b, (size_t)hl + 1, "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n", hl, (int)total, sf, ef);
    memcpy(b + hl, pre, sizeof pre - 1);
    memcpy(b + sf, p, n);
    memcpy(b + ef, post, sizeof post - 1);
    b[total] = 0;
    HGLOBAL g = gdup(b, total + 1);
    free(b);
    return g;
}
/* CF_DIB: a BITMAPINFOHEADER and 32-bit rows, bottom row first */
static HGLOBAL mk_dib(const uint8_t *p, uint32_t n) {
    UINT w = 0, hh = 0;
    uint8_t *px = png_decode(p, n, &w, &hh);
    if (!px) return NULL;
    size_t row = (size_t)w * 4, sz = sizeof(BITMAPINFOHEADER) + row * hh;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sz);
    uint8_t *d = g ? GlobalLock(g) : NULL;
    if (d) {
        BITMAPINFOHEADER bh;
        memset(&bh, 0, sizeof bh);
        bh.biSize = sizeof bh; bh.biWidth = (LONG)w; bh.biHeight = (LONG)hh; bh.biPlanes = 1; bh.biBitCount = 32; bh.biCompression = BI_RGB;
        bh.biSizeImage = (DWORD)(row * hh);
        memcpy(d, &bh, sizeof bh);
        for (UINT y = 0; y < hh; y++) memcpy(d + sizeof bh + row * (hh - 1 - y), px + row * y, row);
        GlobalUnlock(g);
    } else if (g) { GlobalFree(g); g = NULL; }
    free(px);
    return g;
}

/* what the host sent, on the clipboard: every form it holds at once (a host that pushes: no "clipreq") */
static void clip_put(const AgClip *c) {
    uint64_t h = c->png ? hash(c->png, c->png_n) : c->html ? hash(c->html, c->html_n) : hash(c->text, c->text_n);
    if (h == clip_last) return;
    if (!open_clip()) return;
    EmptyClipboard();
    if (c->text) set_fmt(CF_UNICODETEXT, mk_text(c->text, c->text_n));
    if (c->html && cf_html) set_fmt(cf_html, mk_html(c->html, c->html_n));
    if (c->png) {
        if (cf_png) set_fmt(cf_png, gdup(c->png, c->png_n));
        set_fmt(CF_DIB, mk_dib(c->png, c->png_n));
    }
    CloseClipboard();
    clip_last = h;
    lg("clipboard from the host:%s%s%s", c->png ? " picture" : "", c->html ? " html" : "", c->text ? " text" : "");
}

/* ---- the host's clipboard on demand (AGK_CLIPOFFER) ---- */
static uint32_t offer_gen;          /* the offer this side holds on the clipboard (0 none), its forms and flags */
static uint8_t offer_forms, offer_flags;
static struct { int state; uint8_t *data; uint32_t n; } got[4];     /* per AGF_* form: 0 not asked, 1 got, 2 refused */

static void offer_drop(void) {
    for (int i = 0; i < 4; i++) { free(got[i].data); got[i].data = NULL; got[i].n = 0; got[i].state = 0; }
    offer_gen = 0;
}

/* the host's offer on the clipboard: its forms, no data (delayed rendering), marked so that clipboard history, cloud
 * clipboard and clipboard monitors leave it alone */
static void clip_offer(uint32_t gen, uint8_t forms, uint8_t flags) {
    static UINT ex, hist, cloud;
    if (!ex) ex = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (!hist) hist = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (!cloud) cloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    if (!open_clip()) return;
    EmptyClipboard();                   /* our own last offer goes (WM_DESTROYCLIPBOARD: offer_drop) */
    offer_drop();
    offer_gen = gen; offer_forms = forms; offer_flags = flags;
    if (forms & AGM_TEXT) SetClipboardData(CF_UNICODETEXT, NULL);
    if ((forms & AGM_HTML) && cf_html) SetClipboardData(cf_html, NULL);
    if (forms & AGM_PNG) { if (cf_png) SetClipboardData(cf_png, NULL); SetClipboardData(CF_DIB, NULL); }
    DWORD zero = 0;
    if (ex) set_fmt(ex, gdup(&zero, sizeof zero));
    if (hist) set_fmt(hist, gdup(&zero, sizeof zero));
    if (cloud) set_fmt(cloud, gdup(&zero, sizeof zero));
    CloseClipboard();
    clip_last = 0;
    lg("clipboard: the host offers%s%s%s (%u)%s", forms & AGM_TEXT ? " text" : "", forms & AGM_HTML ? " html" : "", forms & AGM_PNG ? " picture" : "", gen,
       flags & AGO_ASK ? ", the person is asked" : "");
}

/* one form of the offer from the host: asked once per offer, waited for at most AG_CLIP_WAIT_MS (AG_ASK_WAIT_MS while
 * the person is asked); refused, stale or no answer: nothing (the paste gets nothing) */
static void fetch(int form) {
    static const char *const NAME[4] = { "", "text", "html", "picture" };
    got[form].state = 2;
    if (!host_hello) return;
    EnterCriticalSection(&ans_cs);
    ans.set = 0;
    LeaveCriticalSection(&ans_cs);
    ResetEvent(ans_ev);
    uint8_t rq[5];
    ag_clipreq_write(rq, offer_gen, (uint8_t)form);
    if (send_frame(AGK_CLIPREQ, rq, 5)) return;
    DWORD wait = offer_flags & AGO_ASK ? AG_ASK_WAIT_MS : AG_CLIP_WAIT_MS, t0 = GetTickCount();
    for (;;) {
        DWORD spent = GetTickCount() - t0;
        if (spent >= wait || port == INVALID_HANDLE_VALUE) { lg("clipboard: no answer from the host for the %s", NAME[form]); return; }
        if (WaitForSingleObject(ans_ev, wait - spent) != WAIT_OBJECT_0) continue;
        EnterCriticalSection(&ans_cs);
        int mine = ans.set && ans.gen == offer_gen && ans.form == form;
        if (mine) {
            ans.set = 0;
            if (ans.status == AGD_OK) { got[form].data = ans.data; got[form].n = ans.n; got[form].state = 1; ans.data = NULL; }
            else lg("clipboard: the host didn't give the %s (%s)", NAME[form], ans.status == AGD_STALE ? "it changed" : ans.status == AGD_NONE ? "not there" : "refused");
        }
        ResetEvent(ans_ev);
        LeaveCriticalSection(&ans_cs);
        if (mine) return;
    }
}

/* WM_RENDERFORMAT: a program pastes one of the offer's forms */
static void clip_render(UINT fmt) {
    int form = fmt == CF_UNICODETEXT ? AGF_TEXT : (cf_html && fmt == cf_html) ? AGF_HTML : ((cf_png && fmt == cf_png) || fmt == CF_DIB) ? AGF_PNG : 0;
    if (!form || !offer_gen) return;
    if (!got[form].state) fetch(form);
    if (got[form].state != 1) return;
    const uint8_t *d = got[form].data;
    uint32_t n = got[form].n;
    HGLOBAL g = form == AGF_TEXT ? mk_text(d, n) : form == AGF_HTML ? mk_html(d, n) : fmt == CF_DIB ? mk_dib(d, n) : gdup(d, n);
    if (g && !SetClipboardData(fmt, g)) GlobalFree(g);
}

/* ---- the display's size ---- */
static uint32_t disp_w, disp_h;     /* what the host asked for last */
static int disp_tries;
static DWORD disp_got_w, disp_got_h; /* what that ask ended with (0 still trying) */
static int disp_again;              /* asked again after Windows changed it by itself, for this ask */
static ULONGLONG disp_asked_at;

static int primary_device(wchar_t name[32]) {
    DISPLAY_DEVICEW d;
    for (DWORD i = 0;; i++) {
        memset(&d, 0, sizeof d);
        d.cb = sizeof d;
        if (!EnumDisplayDevicesW(NULL, i, &d, 0)) return -1;
        if (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { memcpy(name, d.DeviceName, sizeof d.DeviceName); return 0; }
    }
}

/* ---- the guest's displays, and the one the machine's picture shows ----
 * Windows may have more than one display on its desktop while the machine's window shows one picture: an Intel GVT-g
 * vGPU's output (PCI) can sit next to a basic or indirect display that isn't on a PCI adapter (on the maintainer's
 * guest: a 1022x724 primary nobody sees). Everything this side does with the picture's coordinates (the size the host
 * asks for, apps mode's list and moves, a drop's point) is done on the "shown" display, chosen by ag_shown_pick
 * (vmagent.c, tested in vm-test):
 *   - the host's picture only ever comes from a PCI GPU (GVT-g, virtio-gpu, QXL, a passed-through card): while a
 *     display is on a PCI adapter, one that isn't is never chosen;
 *   - among those: the one chosen before by size while it stays; else the one whose mode is the size the host asked
 *     for (AGK_DISPLAY: equal, or under it by less than 8 pixels), the primary first; else the primary; else the first;
 *   - a change of which displays there are (by name) forgets the choice made by size.
 * The shown display is then made Windows' primary (disp_make_primary), so the taskbar, the desktop's icons, new windows
 * and a consent prompt (UAC) are where the person looks, and windows on another display come onto it (rescue_all) as
 * the tools start, after that change, and when apps mode ends. Nothing is detached: the other display stays, to the
 * shown one's right, so nothing of the person's setup is lost and a wrong guess costs only a primary that can be set
 * back in Settings. The list and the choice go to this log and the host's (AGK_DISPLAYS) whenever either changes. */
typedef struct { wchar_t dev[32]; RECT r, work; int primary, hw; } Mon;
static Mon mons[AG_DISPLAYS_MAX];
static int mons_n, mons_shown = -1, mons_stale = 1;
static wchar_t shown_locked[32];    /* chosen by the host's size, "" not yet */
static wchar_t mons_topo[AG_DISPLAYS_MAX * 33];   /* the displays' names, to tell a change of topology */
static wchar_t primary_tried[32];   /* made primary already (for this topology), "" not yet: for the log */
static ULONGLONG primary_at[5];     /* when the shown display was last made primary (the last five), to keep it so */
static int rescue_pending = 1;      /* windows on another display come onto the shown one when things settle */
static char mons_said[AG_DISPLAYS_MAX * 128];

/* the adapter that drives a display: 1 a PCI device; its description into desc */
static int adapter_of(const wchar_t *dev, wchar_t *desc, size_t cap) {
    DISPLAY_DEVICEW d;
    if (cap) desc[0] = 0;
    for (DWORD i = 0; i < 64; i++) {
        memset(&d, 0, sizeof d);
        d.cb = sizeof d;
        if (!EnumDisplayDevicesW(NULL, i, &d, 0)) break;
        if (wcscmp(d.DeviceName, dev)) continue;
        if (cap) { wcsncpy(desc, d.DeviceString, cap - 1); desc[cap - 1] = 0; }
        return !_wcsnicmp(d.DeviceID, L"PCI\\", 4);
    }
    return 0;
}

static BOOL CALLBACK mon_one(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp) {
    (void)dc; (void)rc;
    int *n = (int *)lp;
    if (*n >= AG_DISPLAYS_MAX) return FALSE;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi)) return TRUE;
    Mon *m = &mons[*n];
    memset(m, 0, sizeof *m);
    wcsncpy(m->dev, mi.szDevice, 31);
    m->r = mi.rcMonitor;
    m->work = mi.rcWork;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m->hw = adapter_of(m->dev, NULL, 0);
    (*n)++;
    return TRUE;
}

static void mon_as_agd(const Mon *m, AgDisplay *o) {
    memset(o, 0, sizeof *o);
    size_t k = 0;
    for (; k < AG_DISPLAY_NAME_MAX && m->dev[k]; k++) o->name[k] = m->dev[k] >= 0x20 && m->dev[k] <= 0x7e ? (char)m->dev[k] : '?';
    if (!k) o->name[k++] = '?';
    o->x = m->r.left; o->y = m->r.top;
    LONG w = m->r.right - m->r.left, h = m->r.bottom - m->r.top;
    o->w = w > 0 ? (uint32_t)w : 0; o->h = h > 0 ? (uint32_t)h : 0;
    o->flags = (uint8_t)((m->primary ? AGDS_PRIMARY : 0) | (m->hw ? AGDS_HW : 0));
}

static int mon_pick(void) {
    AgDisplay d[AG_DISPLAYS_MAX];
    int locked = -1, lock = -1;
    for (int i = 0; i < mons_n; i++) {
        mon_as_agd(&mons[i], &d[i]);
        if (shown_locked[0] && !wcscmp(mons[i].dev, shown_locked)) locked = i;
    }
    int k = ag_shown_pick(d, mons_n, disp_w, disp_h, locked, &lock);
    if (lock >= 0) memcpy(shown_locked, mons[lock].dev, sizeof shown_locked);
    else shown_locked[0] = 0;
    return k;
}

/* The shown display made Windows' primary: its source at 0,0 and the others in a row to its right, through the display
 * configuration (what Settings' "Make this my main display" does), saved. 0 done */
static int disp_make_primary(const wchar_t *dev) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np || !nm || np > 64 || nm > 128) return -1;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
    int r = -1;
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        int shown = -1;
        UINT32 sw = 0;
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            UINT32 ix = paths[i].sourceInfo.modeInfoIdx;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, dev)) continue;
            if (ix >= nm || modes[ix].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) break;
            shown = (int)ix;
            sw = modes[ix].sourceMode.width;
            break;
        }
        if (shown >= 0) {
            /* every other source to its right, in the order Windows lists them (a source shared by two paths once) */
            LONG x = (LONG)sw;
            modes[shown].sourceMode.position.x = 0;
            modes[shown].sourceMode.position.y = 0;
            for (UINT32 i = 0; i < nm; i++) {
                if ((int)i == shown || modes[i].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
                modes[i].sourceMode.position.x = x;
                modes[i].sourceMode.position.y = 0;
                x += (LONG)modes[i].sourceMode.width;
            }
            LONG e = SetDisplayConfig(np, paths, nm, modes, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);
            r = e ? -1 : 0;
            char *dn = to_utf8(dev, -1, NULL);
            if (e) lg("displays: making %s the primary display was refused (%ld)", dn ? dn : "?", e);
            else lg("displays: %s is now the primary display (the taskbar, the desktop and new windows go there); the others to its right", dn ? dn : "?");
            free(dn);
        }
    }
    free(paths);
    free(modes);
    return r;
}

/* ---- the extra display (AGK_EXTRA) ----
 * A display that isn't on a PCI adapter (a basic or indirect display) next to the PCI one the host shows gets the
 * tablet's clicks, windows and the focus out of sight. Not turned off by itself: what it is isn't known for sure (its
 * adapter is logged), so the person decides, from Machines (the agent's popover). Off: its paths are left out of the
 * display configuration (never the shown one's, so at least one stays), saved; remembered in
 * HKCU\Software\LordOS\GuestWin ExtraDisplaysOff. Back on: Windows extends the desktop over every connected display
 * again (SDC_TOPOLOGY_EXTEND), also by itself when no PCI display is left. */
static void mon_get(void);
static int extra_is_off(void) {
    DWORD v = 0, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\LordOS\\GuestWin", L"ExtraDisplaysOff", RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS && v;
}
static void extra_mark(int off) {
    DWORD v = (DWORD)off;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\LordOS\\GuestWin", L"ExtraDisplaysOff", REG_DWORD, &v, sizeof v);
}

static int extra_off(void) {
    mon_get();
    if (mons_shown < 0 || !mons[mons_shown].hw) { lg("displays: no extra display turned off: the shown one isn't on a PCI adapter"); return -1; }
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np || !nm || np > 64 || nm > 128) return -1;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths), *keep = calloc(np, sizeof *keep);
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes), *km = calloc(nm, sizeof *km);
    int r = -1;
    if (paths && keep && modes && km && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        UINT32 kp = 0, kn = 0, dropped = 0;
        int shown_kept = 0;
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            int drop = 0;
            if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) {
                for (int k = 0; k < mons_n; k++)
                    if (k != mons_shown && !mons[k].hw && !wcscmp(mons[k].dev, sn.viewGdiDeviceName)) drop = 1;
                if (!wcscmp(mons[mons_shown].dev, sn.viewGdiDeviceName)) shown_kept = 1;
            }
            if (drop) {
                wchar_t desc[128];
                adapter_of(sn.viewGdiDeviceName, desc, 128);
                char *dn = to_utf8(sn.viewGdiDeviceName, -1, NULL), *ds = to_utf8(desc, -1, NULL);
                lg("displays: turning off %s (%s, not on a PCI adapter)", dn ? dn : "?", ds ? ds : "?");
                free(dn); free(ds);
                dropped++;
                continue;
            }
            /* a kept path, with its modes copied and its indexes set again */
            keep[kp] = paths[i];
            UINT32 si = paths[i].sourceInfo.modeInfoIdx, ti = paths[i].targetInfo.modeInfoIdx;
            if (si < nm) { km[kn] = modes[si]; keep[kp].sourceInfo.modeInfoIdx = kn++; }
            if (ti < nm) { km[kn] = modes[ti]; keep[kp].targetInfo.modeInfoIdx = kn++; }
            kp++;
        }
        if (!dropped) lg("displays: no extra display to turn off");
        else if (!shown_kept || !kp) lg("displays: the shown display isn't in the configuration: nothing turned off");
        else {
            LONG e = SetDisplayConfig(kp, keep, kn, km, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);
            if (e) lg("displays: turning the extra display off was refused (%ld)", e);
            else { lg("displays: %u extra display(s) off; Machines can turn them back on", dropped); extra_mark(1); r = 0; }
        }
    }
    free(paths); free(keep); free(modes); free(km);
    return r;
}

static int extra_on(const char *why) {
    LONG e = SetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    lg("displays: every display back on (%s): %ld", why, e);
    if (e) return -1;
    extra_mark(0);
    primary_tried[0] = 0;               /* the shown one made the primary again if Windows moved it */
    return 0;
}

/* this log: every display adapter and its monitors, attached to the desktop or not (once per change) */
static void mon_log_adapters(void) {
    DISPLAY_DEVICEW a, m;
    for (DWORD i = 0; i < 64; i++) {
        memset(&a, 0, sizeof a);
        a.cb = sizeof a;
        if (!EnumDisplayDevicesW(NULL, i, &a, 0)) break;
        if (!(a.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && !(a.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
            /* an output with nothing on the desktop: said only when a monitor is connected to it */
            memset(&m, 0, sizeof m);
            m.cb = sizeof m;
            if (!EnumDisplayDevicesW(a.DeviceName, 0, &m, 0)) continue;
        }
        char *dn = to_utf8(a.DeviceName, -1, NULL), *ds = to_utf8(a.DeviceString, -1, NULL), *id = to_utf8(a.DeviceID, -1, NULL);
        lg("displays:   %s: %s [%s] flags 0x%lx", dn ? dn : "?", ds ? ds : "?", id ? id : "?", (unsigned long)a.StateFlags);
        free(dn); free(ds); free(id);
        for (DWORD j = 0; j < 8; j++) {
            memset(&m, 0, sizeof m);
            m.cb = sizeof m;
            if (!EnumDisplayDevicesW(a.DeviceName, j, &m, 0)) break;
            char *mn = to_utf8(m.DeviceString, -1, NULL);
            lg("displays:     monitor %lu: %s flags 0x%lx", (unsigned long)j, mn ? mn : "?", (unsigned long)m.StateFlags);
            free(mn);
        }
    }
}

/* the displays on the desktop, looked at again when something may have changed; the list goes to the host when it (or
 * the choice) changed, or hasn't gone since the port opened */
static void mon_scan(void) {
    mons_stale = 0;
    int n = 0;
    EnumDisplayMonitors(NULL, NULL, mon_one, (LPARAM)&n);
    mons_n = n;
    /* which displays there are: a change forgets the choice made by size and the primary made */
    wchar_t topo[sizeof mons_topo / sizeof *mons_topo];
    topo[0] = 0;
    for (int i = 0; i < mons_n; i++) { wcsncat(topo, mons[i].dev, 31); wcscat(topo, L"|"); }
    if (wcscmp(topo, mons_topo)) { wcscpy(mons_topo, topo); shown_locked[0] = 0; primary_tried[0] = 0; }
    mons_shown = mon_pick();
    AgDisplay d[AG_DISPLAYS_MAX];
    int dn = 0, ds = -1;
    for (int i = 0; i < mons_n; i++) {
        AgDisplay *o = &d[dn];
        mon_as_agd(&mons[i], o);
        if (o->w < 1 || o->h < 1 || o->w > AG_DISPLAY_MAX || o->h > AG_DISPLAY_MAX || o->x < -LAG_DIM_MAX || o->x > LAG_DIM_MAX ||
            o->y < -LAG_DIM_MAX || o->y > LAG_DIM_MAX) continue;
        if (i == mons_shown) ds = dn;
        dn++;
    }
    char t[sizeof mons_said];
    if (dn) ag_displays_text(d, dn, ds, t, sizeof t);
    else snprintf(t, sizeof t, "none");
    if (strcmp(t, mons_said)) {
        memcpy(mons_said, t, sizeof t);
        lg("displays: %s%s", t, mons_n > 1 && shown_locked[0] ? " (the shown one chosen by the size the host asked for)" : "");
        mon_log_adapters();
        mons_sent = 0;
        if (apps_on) apps_dirty = apps_force = 1;   /* the list again, relative to the shown display as it is now, sent
                                                     * even when nothing in it changed (the host waits for it) */
    }
    if (!mons_sent && dn && port != INVALID_HANDLE_VALUE && host_hello) {
        uint8_t b[AG_DISPLAYS_BYTES];
        size_t bn = ag_displays_write(d, dn, ds, b, sizeof b);
        if (bn && !send_frame(AGK_DISPLAYS, b, (uint32_t)bn)) mons_sent = 1;
    }
    /* The shown display made the primary (only one on a PCI adapter: the host's picture), and kept so: a change of mode
     * on it (ChangeDisplaySettingsEx, a monitor that comes back with a new EDID) lets Windows put another display at
     * 0,0 as the primary, and then the tablet, the taskbar and new windows go where nobody looks (w9 made it primary
     * once per set of displays only; 2026-11-22 log: the layout flipped on every size). Not more than once in 2 s, nor
     * five times in a minute: a Windows that keeps undoing it isn't fought with */
    if (mons_n > 1 && mons_shown >= 0 && mons[mons_shown].hw && !mons[mons_shown].primary) {
        ULONGLONG now = GetTickCount64();
        if (now - primary_at[0] >= 2000 && (!primary_at[4] || now - primary_at[4] >= 60000)) {
            memmove(primary_at + 1, primary_at, sizeof primary_at - sizeof *primary_at);
            primary_at[0] = now;
            int again = !wcscmp(primary_tried, mons[mons_shown].dev);
            memcpy(primary_tried, mons[mons_shown].dev, sizeof primary_tried);
            if (again) lg("displays: another display became the primary: the shown one is made primary again");
            if (!disp_make_primary(mons[mons_shown].dev)) {
                mons_stale = 1;
                rescue_pending = 1;             /* the windows come onto it once the change settles */
                SetTimer(win, T_MONS, 1500, NULL);
            }
        } else if (!(now - primary_at[0] < 2000)) {
            static ULONGLONG told;
            if (now - told > 60000) { told = now; lg("displays: Windows made another display the primary five times in a minute: left so for now"); }
        } else SetTimer(win, T_MONS, 2100, NULL);   /* looked at again once the 2 s are over */
    }
    /* an extra display was turned off but no display on a PCI adapter is left: everything back on, once */
    static int extra_auto_tried;
    int anyhw = 0;
    for (int i = 0; i < mons_n; i++) anyhw |= mons[i].hw;
    if (mons_n && !anyhw && !extra_auto_tried && extra_is_off()) {
        extra_auto_tried = 1;
        if (!extra_on("no display on a PCI adapter is left") && port != INVALID_HANDLE_VALUE) say_hello();
    }
}

static void mon_get(void) { if (mons_stale) mon_scan(); }

/* the shown display's device name (disp_*), or the primary's when Windows lists none */
static int shown_device(wchar_t name[32]) {
    mon_get();
    if (mons_shown >= 0) { memcpy(name, mons[mons_shown].dev, sizeof mons[mons_shown].dev); return 0; }
    return primary_device(name);
}

/* the shown display's rectangle and work area in screen coordinates (apps mode), the primary's when none is known */
static void shown_rect(RECT *r, RECT *work) {
    mon_get();
    if (mons_shown >= 0) { *r = mons[mons_shown].r; if (work) *work = mons[mons_shown].work; return; }
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    POINT o = { 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi)) { *r = mi.rcMonitor; if (work) *work = mi.rcWork; return; }
    SetRect(r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (work) *work = *r;
}

/* the driver's custom mode, asked for directly (viogpudo from 2026: VIOGPU_SET_CUSTOM_RESOLUTION). Declared here: the
 * D3DKMT structures as the WDK has them; the functions are gdi32's */
typedef UINT D3DKMT_HANDLE;
typedef struct { HDC hDc; D3DKMT_HANDLE hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } KmtOpen;
typedef struct { D3DKMT_HANDLE hAdapter, hDevice; UINT Type; UINT Flags; void *pPrivateDriverData; UINT PrivateDriverDataSize; D3DKMT_HANDLE hContext; } KmtEscape;
#pragma pack(push, 1)
typedef struct { USHORT Type, DataLength; union { ULONG Id; struct { USHORT X, Y; } Res; } u; } VioEscape;
#pragma pack(pop)
static void viogpu_ask(const wchar_t *dev, uint32_t w, uint32_t h) {
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    typedef LONG (APIENTRY *OpenFn)(KmtOpen *);
    typedef LONG (APIENTRY *EscFn)(const KmtEscape *);
    typedef LONG (APIENTRY *CloseFn)(const D3DKMT_HANDLE *);
    OpenFn op = g ? (OpenFn)(void *)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc") : NULL;
    EscFn es = g ? (EscFn)(void *)GetProcAddress(g, "D3DKMTEscape") : NULL;
    CloseFn cl = g ? (CloseFn)(void *)GetProcAddress(g, "D3DKMTCloseAdapter") : NULL;
    if (!op || !es || !cl) return;
    HDC dc = CreateDCW(NULL, dev, NULL, NULL);
    if (!dc) return;
    KmtOpen o;
    memset(&o, 0, sizeof o);
    o.hDc = dc;
    if (!op(&o)) {
        VioEscape v;
        memset(&v, 0, sizeof v);
        v.Type = 2;                                     /* VIOGPU_SET_CUSTOM_RESOLUTION */
        v.DataLength = 4;
        v.u.Res.X = (USHORT)w; v.u.Res.Y = (USHORT)h;
        KmtEscape e;
        memset(&e, 0, sizeof e);
        e.hAdapter = o.hAdapter;
        e.pPrivateDriverData = &v;
        e.PrivateDriverDataSize = sizeof v;
        LONG r = es(&e);
        lg("display: asked the driver for %ux%u: status 0x%lx", w, h, (unsigned long)r);
        cl(&o.hAdapter);
    }
    DeleteDC(dc);
}

/* the size through the display configuration, as virtio-win's viogpuap does: Windows asks the driver again (its custom
 * mode, which follows the host) instead of choosing from the list it read when the monitor appeared. 0 asked */
static int disp_setconfig(const wchar_t *dev, uint32_t w, uint32_t h) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS || !np || !nm || np > 64 || nm > 128) return -1;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
    int r = -1;
    if (paths && modes && QueryDisplayConfig(QDC_ALL_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, dev)) continue;
            UINT32 ix = paths[i].sourceInfo.modeInfoIdx;
            if (ix >= nm || modes[ix].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) break;
            modes[ix].sourceMode.width = w;
            modes[ix].sourceMode.height = h;
            LONG e = SetDisplayConfig(np, paths, nm, modes, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
            if (e) lg("display: the display configuration refused %ux%u (%ld)", w, h, e);
            r = e ? -1 : 0;
            break;
        }
    }
    free(paths);
    free(modes);
    return r;
}

/* one try: the exact size when the driver lists it; at the last try, the largest listed mode that fits */
static int disp_apply(int last) {
    wchar_t dev[32];
    if (shown_device(dev)) return -1;
    DEVMODEW cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &cur, 0)) return -1;
    if (cur.dmPelsWidth <= disp_w && cur.dmPelsHeight <= disp_h && disp_w - cur.dmPelsWidth < 8 && disp_h - cur.dmPelsHeight < 8) return 1;
    DEVMODEW m, best;
    int found = 0, have_best = 0;
    for (DWORD i = 0;; i++) {
        memset(&m, 0, sizeof m);
        m.dmSize = sizeof m;
        if (!EnumDisplaySettingsExW(dev, i, &m, 0)) break;
        if (m.dmBitsPerPel != cur.dmBitsPerPel) continue;
        /* the driver's custom mode may come rounded down (a width of whole 8-pixel groups): within 8 counts */
        if (m.dmPelsWidth <= disp_w && m.dmPelsHeight <= disp_h && disp_w - m.dmPelsWidth < 8 && disp_h - m.dmPelsHeight < 8) { found = 1; best = m; break; }
        if (m.dmPelsWidth <= disp_w && m.dmPelsHeight <= disp_h &&
            (!have_best || (uint64_t)m.dmPelsWidth * m.dmPelsHeight > (uint64_t)best.dmPelsWidth * best.dmPelsHeight)) { best = m; have_best = 1; }
    }
    if (!found && !last) {
        /* not listed (yet): Windows is asked for it through the display configuration, from the second try on */
        /* twice at most (each one is a mode set the person sees: every 250 ms it flickered for 4 s) */
        if ((disp_tries == 2 || disp_tries == 8) && !disp_setconfig(dev, disp_w, disp_h)) {
            memset(&cur, 0, sizeof cur);
            cur.dmSize = sizeof cur;
            if (EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &cur, 0) && cur.dmPelsWidth <= disp_w && cur.dmPelsHeight <= disp_h &&
                disp_w - cur.dmPelsWidth < 8 && disp_h - cur.dmPelsHeight < 8) { lg("display: %lux%lu (the driver's own mode)", cur.dmPelsWidth, cur.dmPelsHeight); return 1; }
        }
        return 0;
    }
    if (!found && logf) {
        char l[1024];
        size_t o = 0;
        for (DWORD i = 0; o + 24 < sizeof l; i++) {
            memset(&m, 0, sizeof m);
            m.dmSize = sizeof m;
            if (!EnumDisplaySettingsExW(dev, i, &m, 0)) break;
            if (m.dmBitsPerPel == cur.dmBitsPerPel) o += (size_t)snprintf(l + o, sizeof l - o, " %lux%lu", m.dmPelsWidth, m.dmPelsHeight);
        }
        l[o] = 0;
        lg("display: %ux%u isn't listed:%s", disp_w, disp_h, l);
    }
    if (!found && !have_best) { lg("display: no mode fits %ux%u", disp_w, disp_h); return -1; }
    DEVMODEW want;
    memset(&want, 0, sizeof want);
    want.dmSize = sizeof want;
    want.dmPelsWidth = best.dmPelsWidth;
    want.dmPelsHeight = best.dmPelsHeight;
    want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (best.dmPelsWidth == cur.dmPelsWidth && best.dmPelsHeight == cur.dmPelsHeight) return 1;
    LONG r = ChangeDisplaySettingsExW(dev, &want, NULL, CDS_UPDATEREGISTRY, NULL);
    lg("display: %lux%lu%s: %ld", best.dmPelsWidth, best.dmPelsHeight, found ? "" : " (the closest listed)", r);
    /* Windows may have moved the displays around for the new size (another one primary at 0,0): put back at once */
    if (r == DISP_CHANGE_SUCCESSFUL) { mons_stale = 1; mon_get(); }
    return r == DISP_CHANGE_SUCCESSFUL ? 1 : -1;
}

static void disp_report(void) {
    wchar_t dev[32];
    DEVMODEW cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    mons_stale = 1;
    if (shown_device(dev) || !EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &cur, 0)) return;
    disp_got_w = cur.dmPelsWidth; disp_got_h = cur.dmPelsHeight;
    uint8_t o[8];
    if (cur.dmPelsWidth < AG_DISPLAY_MIN_W || cur.dmPelsHeight < AG_DISPLAY_MIN_H || cur.dmPelsWidth > AG_DISPLAY_MAX || cur.dmPelsHeight > AG_DISPLAY_MAX) return;
    ag_display_write(o, cur.dmPelsWidth, cur.dmPelsHeight);
    char *dn = to_utf8(dev, -1, NULL);
    lg("display: %s is %lux%lu", dn ? dn : "?", cur.dmPelsWidth, cur.dmPelsHeight);
    free(dn);
    send_frame(AGK_DISPLAY, o, 8);
}

/* The size the host asked for, settled, changed by Windows itself a moment later: a vGPU's monitor comes back with a new
 * EDID after QEMU's ui_info, and Windows then applies the mode it remembers for that monitor (2026-11-22 log: 1920x1080
 * set, 1400x1050 half a second later, and it stayed). Asked again, twice at most per ask and within 30 s of it, and only
 * when it isn't what this side itself ended with (the closest listed mode) */
static void disp_keep(void) {
    if (!disp_w || !disp_got_w || disp_again >= 2 || GetTickCount64() - disp_asked_at > 30000) return;
    wchar_t dev[32];
    DEVMODEW cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (shown_device(dev) || !EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &cur, 0)) return;
    if (cur.dmPelsWidth == disp_got_w && cur.dmPelsHeight == disp_got_h) return;
    disp_again++;
    lg("display: Windows changed the size to %lux%lu by itself after %lux%lu was set: %ux%u asked again", cur.dmPelsWidth, cur.dmPelsHeight,
       disp_got_w, disp_got_h, disp_w, disp_h);
    disp_got_w = disp_got_h = 0;
    disp_tries = 0;
    SetTimer(win, T_DISP, 250, NULL);
}

/* the host's size arrives with (or just before) the driver's own change: the mode is looked for every 250 ms for 4 s */
static void disp_tick(void) {
    disp_tries++;
    mons_stale = 1;                 /* the mode may have changed since the last try */
    if (disp_tries == 4) { wchar_t dev[32]; if (!shown_device(dev)) viogpu_ask(dev, disp_w, disp_h); }
    int r = disp_apply(disp_tries >= 16);
    if (r != 0) { KillTimer(win, T_DISP); disp_report(); }
}

/* ---- how Windows maps the absolute tablet (AGK_TABLET) ----
 * QEMU's USB tablet is a HID mouse with absolute X and Y (mouhid). Whether Windows spreads its range over the whole
 * desktop (every display) or the primary display alone decides where a click lands when the guest has more than one
 * display, and the host has to know it to aim. Windows says it in the input itself: MOUSE_VIRTUAL_DESKTOP set on an
 * absolute move means the whole desktop, unset the primary display (RAWMOUSE, the same flags mouclass hands win32k).
 * This window reads the mice's raw input in the background (RIDEV_INPUTSINK: it takes nothing from anyone), notes the
 * mapping of the first absolute move and of any change, and says it to the host. The first few moves are logged with
 * where Windows put the pointer (GetCursorPos a moment later) against both readings, so the log shows it measured. */
static int tablet_logged;
static LONG tablet_raw_x, tablet_raw_y;
static int tablet_raw_flags;
static void tablet_say(void) {
    if (!tablet_mode || tablet_sent || port == INVALID_HANDLE_VALUE || !host_hello) return;
    uint8_t b = (uint8_t)tablet_mode;
    if (!send_frame(AGK_TABLET, &b, 1)) tablet_sent = 1;
}
static void tablet_input(HRAWINPUT ri) {
    RAWINPUT r;
    UINT n = sizeof r;
    if (GetRawInputData(ri, RID_INPUT, &r, &n, sizeof r.header) == (UINT)-1 || r.header.dwType != RIM_TYPEMOUSE) return;
    USHORT f = r.data.mouse.usFlags;
    if (!(f & MOUSE_MOVE_ABSOLUTE)) return;
    int mode = f & MOUSE_VIRTUAL_DESKTOP ? AG_TABLET_DESKTOP : AG_TABLET_PRIMARY;
    if (mode != tablet_mode) {
        lg("tablet: Windows maps the absolute tablet over %s (raw input flags 0x%x)", mode == AG_TABLET_DESKTOP ? "the whole desktop" : "the primary display", f);
        tablet_mode = mode;
        tablet_sent = 0;
        tablet_logged = 0;
    }
    tablet_say();
    if (tablet_logged < 3) {
        tablet_raw_x = r.data.mouse.lLastX; tablet_raw_y = r.data.mouse.lLastY; tablet_raw_flags = f;
        SetTimer(win, T_TABLET, 60, NULL);
    }
}
/* a moment after a move: where Windows put the pointer, against what each mapping gives for that raw position */
static void tablet_check(void) {
    if (tablet_logged >= 3) return;
    tablet_logged++;
    POINT c;
    if (!GetCursorPos(&c)) return;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int pw = GetSystemMetrics(SM_CXSCREEN), ph = GetSystemMetrics(SM_CYSCREEN);
    long dx = vx + (long)((long long)tablet_raw_x * vw / 65536), dy = vy + (long)((long long)tablet_raw_y * vh / 65536);
    long px = (long)((long long)tablet_raw_x * pw / 65536), py = (long)((long long)tablet_raw_y * ph / 65536);
    lg("tablet: raw %ld,%ld (flags 0x%x): the pointer is at %ld,%ld; the whole desktop (%dx%d at %d,%d) gives %ld,%ld, the primary (%dx%d) %ld,%ld",
       tablet_raw_x, tablet_raw_y, tablet_raw_flags, c.x, c.y, vw, vh, vx, vy, dx, dy, pw, ph, px, py);
}

/* ---- apps mode ---- */
static HWINEVENTHOOK hooks[2];
static uint32_t win_serial;
static uint32_t listed[LAG_WIN_MAX];
static int listed_n;
static Buf last_list;
static uint32_t iconed[256];
static int iconed_n;
static int taskbar_was;             /* ABM_GETSTATE before apps mode */
typedef struct { char *name, *token; } App;
static App *apps;
static int apps_n;

static int cloaked(HWND h) {
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof c)) && c;
}

/* A window whose program runs with more rights than this helper (Windows: "as administrator", a high integrity level;
 * or SYSTEM): the system drops this side's messages to it and refuses its moves (UIPI), so it can't be closed, moved,
 * minimized or restored from here. This helper is never elevated to get around that. */
static DWORD token_il(HANDLE tok) {
    BYTE buf[64];
    DWORD n = 0;
    if (!GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof buf, &n)) return 0;
    TOKEN_MANDATORY_LABEL *l = (TOKEN_MANDATORY_LABEL *)buf;
    PUCHAR k = GetSidSubAuthorityCount(l->Label.Sid);
    return k && *k ? *GetSidSubAuthority(l->Label.Sid, (DWORD)(*k - 1)) : 0;
}

static DWORD my_il;
static int elevated_pid(DWORD pid) {
    if (!pid) return 0;
    if (!my_il) {
        HANDLE t;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) { my_il = token_il(t); CloseHandle(t); }
        if (!my_il) my_il = SECURITY_MANDATORY_MEDIUM_RID;
    }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;                   /* not known: treated as not elevated (an action still answers if refused) */
    HANDLE t;
    int up = 0;
    if (OpenProcessToken(p, TOKEN_QUERY, &t)) { DWORD il = token_il(t); up = il > my_il; CloseHandle(t); }
    else up = GetLastError() == ERROR_ACCESS_DENIED;   /* an elevated program's token isn't ours to read */
    CloseHandle(p);
    return up;
}

/* an action the system refused (LAG_WINREFUSED), said in this log once per window and action in a row */
static void win_refused(uint32_t id, uint8_t action, uint8_t reason) {
    static uint32_t last_id;
    static uint8_t last_act;
    static const char *const act[] = { "", "focus", "close", "minimize", "restore", "move" };
    if (id != last_id || action != last_act)
        lg("apps: window %lx: %s refused (%s)", (unsigned long)id, act[action], reason == LAGR_ELEVATED ? "it runs as administrator" : "access denied");
    last_id = id; last_act = action;
    uint8_t b[LAG_WINREFUSED_BYTES];
    if (!lag_winrefused_write(b, id, action, reason)) send_frame(LAG_WINREFUSED, b, sizeof b);
}

static int listable(HWND h) {
    if (!IsWindowVisible(h) || cloaked(h)) return 0;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return 0;
    if (ex & WS_EX_NOACTIVATE && !(ex & WS_EX_APPWINDOW)) return 0;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64)) {
        static const wchar_t *const skip[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"Windows.UI.Core.CoreWindow", NULL };
        for (int i = 0; skip[i]; i++) if (!wcscmp(cls, skip[i])) return 0;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid != GetCurrentProcessId();
}

static void app_name(DWORD pid, char *out, size_t cap) {
    out[0] = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t *b = wcsrchr(path, L'\\');
        char *u = to_utf8(b ? b + 1 : path, -1, NULL);
        if (u) {
            for (char *q = u; *q; q++) if (*q == '/' || *q == ':') *q = '_';
            snprintf(out, cap, "%s", u);
            free(u);
        }
    }
    CloseHandle(p);
}

/* ---- Store apps: ApplicationFrameHost.exe draws the frame of every Store app's window (Calculator, Settings…) and
 * the app's own CoreWindow sits inside it, in the app's process. Such a window is reported with the app's name and
 * icon (from its id, the AppUserModelID, looked up in shell:AppsFolder), so each app is its own Dock entry. ---- */
static const PROPERTYKEY PK_AUMID = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
enum { AUMID_MAX = 260 };

typedef struct { DWORD host; DWORD pid; } CoreFind;
static BOOL CALLBACK core_one(HWND h, LPARAM lp) {
    CoreFind *c = (CoreFind *)lp;
    wchar_t cls[64];
    DWORD pid = 0;
    if (!GetClassNameW(h, cls, 64) || wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return TRUE;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == c->host) return TRUE;
    c->pid = pid;
    return FALSE;
}

/* the hosted app's id: the frame's own property (set while the app runs, kept while it's suspended), else the id of
 * the process that owns the CoreWindow inside it. 0 found */
static int hosted_aumid(HWND h, DWORD host, wchar_t out[AUMID_MAX]) {
    out[0] = 0;
    IPropertyStore *ps = NULL;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, &IID_IPropertyStore, (void **)&ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PK_AUMID, &v)) && v.vt == VT_LPWSTR && v.pwszVal && wcslen(v.pwszVal) < AUMID_MAX)
            wcscpy(out, v.pwszVal);
        PropVariantClear(&v);
        IPropertyStore_Release(ps);
    }
    if (out[0]) return 0;
    CoreFind c = { host, 0 };
    EnumChildWindows(h, core_one, (LPARAM)&c);
    if (!c.pid) return -1;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c.pid);
    if (!p) return -1;
    UINT32 n = AUMID_MAX;
    LONG r = GetApplicationUserModelId(p, &n, out);
    CloseHandle(p);
    if (r != ERROR_SUCCESS) { out[0] = 0; return -1; }
    return 0;
}

/* the app's name as the Start menu shows it, cached by id (the list is made again on every change) */
typedef struct { wchar_t id[AUMID_MAX]; char name[LAG_APP_MAX + 1]; } AumidName;
static AumidName aumid_names[16];
static int aumid_next;
static int aumid_name(const wchar_t *id, char *out, size_t cap) {
    for (int i = 0; i < 16; i++) if (aumid_names[i].id[0] && !wcscmp(aumid_names[i].id, id)) { snprintf(out, cap, "%s", aumid_names[i].name); return 0; }
    wchar_t path[AUMID_MAX + 20];
    _snwprintf(path, AUMID_MAX + 20, L"shell:AppsFolder\\%ls", id);
    path[AUMID_MAX + 19] = 0;
    IShellItem *it = NULL;
    wchar_t *dn = NULL;
    char *u = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&it)) && SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_NORMALDISPLAY, &dn)))
        u = to_utf8(dn, -1, NULL);
    if (dn) CoTaskMemFree(dn);
    if (it) IShellItem_Release(it);
    if (!u || !*u || strlen(u) > LAG_APP_MAX) { free(u); return -1; }
    for (char *q = u; *q; q++) if (*q == '/' || *q == ':') *q = '_';
    AumidName *e = &aumid_names[aumid_next];
    aumid_next = (aumid_next + 1) % 16;
    wcscpy(e->id, id);
    snprintf(e->name, sizeof e->name, "%s", u);
    snprintf(out, cap, "%s", u);
    free(u);
    return 0;
}

/* the app's icon from its id (the Start menu's own picture, with alpha), 0 drawn */
static HBITMAP aumid_icon(const wchar_t *id, int size) {
    wchar_t path[AUMID_MAX + 20];
    _snwprintf(path, AUMID_MAX + 20, L"shell:AppsFolder\\%ls", id);
    path[AUMID_MAX + 19] = 0;
    IShellItemImageFactory *f = NULL;
    HBITMAP bm = NULL;
    SIZE sz = { size, size };
    if (SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void **)&f))) {
        if (FAILED(IShellItemImageFactory_GetImage(f, sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bm))) bm = NULL;
        IShellItemImageFactory_Release(f);
    }
    return bm;
}

/* windows of ApplicationFrameHost whose app isn't known yet (its frame shows before the app is in it): not listed for
 * a few seconds, so the Dock never gets the host's name; then listed as they are */
typedef struct { HWND h; DWORD since; } Pending;
static Pending pending[16];
static int pending_left;
static int pending_young(HWND h) {
    DWORD now = GetTickCount();
    int free_i = -1;
    for (int i = 0; i < 16; i++) {
        if (pending[i].h == h) return now - pending[i].since < 3000;
        if (!pending[i].h || !IsWindow(pending[i].h)) free_i = i;
    }
    if (free_i < 0) return 0;
    pending[free_i].h = h;
    pending[free_i].since = now;
    return 1;
}

/* the name a window is listed with, and (for a Store app) its id; 1: wait, not listed yet */
static int window_app(HWND h, DWORD pid, char *app, size_t cap, wchar_t aumid[AUMID_MAX]) {
    aumid[0] = 0;
    app_name(pid, app, cap);
    if (_stricmp(app, "ApplicationFrameHost.exe")) return 0;
    if (!hosted_aumid(h, pid, aumid) && !aumid_name(aumid, app, cap)) return 0;
    aumid[0] = 0;
    if (pending_young(h)) { pending_left = 1; return 1; }
    return 0;
}

/* where workspace coordinates (a placement's rectangles) start in screen coordinates: the primary's work area */
static POINT ws_origin(void) {
    POINT o = { 0, 0 };
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi)) { o.x = mi.rcWork.left; o.y = mi.rcWork.top; }
    return o;
}

/* the window's rectangle as seen (without the invisible resize borders), or the restored one for a minimized window;
 * screen coordinates either way */
static int win_rect(HWND h, RECT *r) {
    if (IsIconic(h)) {
        WINDOWPLACEMENT wp;
        wp.length = sizeof wp;
        if (!GetWindowPlacement(h, &wp)) return -1;
        *r = wp.rcNormalPosition;
        if (!(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) { POINT o = ws_origin(); OffsetRect(r, o.x, o.y); }
        return 0;
    }
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof *r)) && !GetWindowRect(h, r)) return -1;
    return 0;
}

/* r (screen coordinates) kept inside area: no larger, then moved in */
static void clamp_into(RECT *r, const RECT *area) {
    LONG w = r->right - r->left, h = r->bottom - r->top;
    if (w > area->right - area->left) w = area->right - area->left;
    if (h > area->bottom - area->top) h = area->bottom - area->top;
    LONG x = r->left, y = r->top;
    if (x + w > area->right) x = area->right - w;
    if (y + h > area->bottom) y = area->bottom - h;
    if (x < area->left) x = area->left;
    if (y < area->top) y = area->top;
    SetRect(r, x, y, x + w, y + h);
}

/* a window's visible frame put at f (screen coordinates): the invisible borders are added back */
static void place_frame(HWND h, const RECT *f) {
    RECT v, w;
    int dl = 0, dt = 0, dr = 0, db = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &v, sizeof v)) && GetWindowRect(h, &w)) {
        dl = v.left - w.left; dt = v.top - w.top; dr = w.right - v.right; db = w.bottom - v.bottom;
    }
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    SetWindowPos(h, NULL, f->left - dl, f->top - dt, f->right - f->left + dl + dr, f->bottom - f->top + dt + db,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

/* A window on a display the machine's picture doesn't show is moved onto the shown one (apps mode, as it starts and for
 * a window that appears later; apps mode off, so nothing stays out of sight): the same place relative to the display's
 * corner, its size kept, then kept inside the shown display's work area. A minimized window stays minimized, only where
 * it comes back moves; a maximized one is maximized on the shown display. A window at least partly on the shown
 * display stays, and so does one on no display at all (a program that parks a window off the screen means it). */
typedef struct { RECT m, work; const char *why; int once; } Rescue;
static HWND rescued[LAG_WIN_MAX * 2];
static int rescued_n;
static BOOL CALLBACK rescue_one(HWND h, LPARAM lp) {
    const Rescue *c = (const Rescue *)lp;
    if (!listable(h)) return TRUE;
    RECT r, k;
    if (win_rect(h, &r) || IsRectEmpty(&r) || IntersectRect(&k, &r, &c->m)) return TRUE;
    HMONITOR src = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!src || !GetMonitorInfoW(src, &mi) || EqualRect(&mi.rcMonitor, &c->m)) return TRUE;
    if (c->once) {
        for (int i = 0; i < rescued_n; i++) if (rescued[i] == h) return TRUE;
        if (rescued_n >= (int)(sizeof rescued / sizeof *rescued)) return TRUE;
        rescued[rescued_n++] = h;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (elevated_pid(pid)) { lg("%s: window %lx runs as administrator: the system won't let it be moved from here", c->why, (unsigned long)(uintptr_t)h); return TRUE; }
    int iconic = IsIconic(h), zoomed = IsZoomed(h);
    RECT t;
    if (iconic || zoomed) {
        WINDOWPLACEMENT wp;
        memset(&wp, 0, sizeof wp);
        wp.length = sizeof wp;
        if (!GetWindowPlacement(h, &wp)) return TRUE;
        POINT o = { 0, 0 };
        if (!(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) o = ws_origin();
        t = wp.rcNormalPosition;
        OffsetRect(&t, o.x, o.y);
        OffsetRect(&t, c->m.left - mi.rcMonitor.left, c->m.top - mi.rcMonitor.top);
        clamp_into(&t, &c->work);
        wp.rcNormalPosition = t;
        OffsetRect(&wp.rcNormalPosition, -o.x, -o.y);
        wp.flags = (wp.flags & WPF_RESTORETOMAXIMIZED) | WPF_ASYNCWINDOWPLACEMENT;
        wp.showCmd = iconic ? SW_SHOWMINNOACTIVE : SW_SHOWMAXIMIZED;
        SetWindowPlacement(h, &wp);
    } else {
        t = r;
        OffsetRect(&t, c->m.left - mi.rcMonitor.left, c->m.top - mi.rcMonitor.top);
        clamp_into(&t, &c->work);
        place_frame(h, &t);
    }
    wchar_t cls[64];
    char *cn = GetClassNameW(h, cls, 64) ? to_utf8(cls, -1, NULL) : NULL;
    lg("%s: window %lx (%s%s) moved from %ld,%ld (another display) to %ld,%ld on the shown one", c->why, (unsigned long)(uintptr_t)h,
       cn ? cn : "?", iconic ? ", minimized: where it comes back" : zoomed ? ", maximized" : "", r.left, r.top, t.left, t.top);
    free(cn);
    return TRUE;
}

/* once: each window tried at most once per apps mode session (rescued[]) */
static void rescue_all(const char *why, int once) {
    mon_get();
    if (mons_n < 2 || mons_shown < 0) return;
    Rescue c;
    memset(&c, 0, sizeof c);
    shown_rect(&c.m, &c.work);
    c.why = why;
    c.once = once;
    EnumWindows(rescue_one, (LPARAM)&c);
}

/* apps mode's coordinates are the shown display's: said in the log once per change of that display */
static RECT apps_said;
static void apps_note(const RECT *m) {
    if (EqualRect(m, &apps_said)) return;
    apps_said = *m;
    lg("apps: windows listed and moved relative to the shown display at %ld,%ld (%ldx%ld): the host's 0,0 is the guest's %ld,%ld",
       m->left, m->top, m->right - m->left, m->bottom - m->top, m->left, m->top);
}

typedef struct { Buf *b; int n; HWND fg; RECT desk; uint32_t ids[LAG_WIN_MAX]; } ListCtx;
static BOOL CALLBACK list_one(HWND h, LPARAM lp) {
    ListCtx *c = (ListCtx *)lp;
    if (c->n >= LAG_WIN_MAX) return FALSE;
    if (!listable(h)) return TRUE;
    RECT r, k;
    if (win_rect(h, &r)) return TRUE;
    if (!IntersectRect(&k, &r, &c->desk)) { if (!IsIconic(h)) return TRUE; k = c->desk; k.right = k.left + 1; k.bottom = k.top + 1; }
    OffsetRect(&k, -c->desk.left, -c->desk.top);          /* relative to the shown display's corner */
    uint32_t id = (uint32_t)(uintptr_t)h;
    if (!id) return TRUE;
    HWND owner = GetWindow(h, GW_OWNER);
    uint32_t oid = (uint32_t)(uintptr_t)owner;
    uint32_t fl = (h == c->fg ? LAGW_FOCUSED : 0) | (IsIconic(h) ? LAGW_MINIMIZED : 0) |
                  (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST ? LAGW_TOPMOST : 0) | (owner && oid && oid != id ? LAGW_OWNED : 0);
    int32_t w = k.right - k.left, hh = k.bottom - k.top;
    if (w < 1 || hh < 1 || w > LAG_DIM_MAX || hh > LAG_DIM_MAX) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (host_elev && elevated_pid(pid)) fl |= LAGW_ELEVATED;
    wchar_t t[300];
    int tl = InternalGetWindowText(h, t, 300);
    char *title = tl > 0 ? to_utf8(t, tl, NULL) : NULL;
    char app[300];
    wchar_t aumid[AUMID_MAX];
    if (window_app(h, pid, app, sizeof app, aumid)) { free(title); return TRUE; }
    b32(c->b, id); b32(c->b, (uint32_t)k.left); b32(c->b, (uint32_t)k.top); b32(c->b, (uint32_t)w); b32(c->b, (uint32_t)hh); b32(c->b, fl);
    if (fl & LAGW_OWNED) b32(c->b, oid);
    b32(c->b, pid);
    bstr(c->b, title ? title : "", LAG_TITLE_MAX);
    bstr(c->b, app, LAG_APP_MAX);
    free(title);
    c->ids[c->n++] = id;
    return TRUE;
}

/* an icon, or a picture with alpha (premultiplied, as the shell gives it), drawn into dc at S×S */
static void draw_icon(HDC dc, HICON ic, HBITMAP bm, int S) {
    if (ic) { DrawIconEx(dc, 0, 0, ic, S, S, 0, NULL, DI_NORMAL); return; }
    BITMAP o;
    HDC src = CreateCompatibleDC(dc);
    if (!src || !GetObjectW(bm, sizeof o, &o)) { if (src) DeleteDC(src); return; }
    HGDIOBJ old = SelectObject(src, bm);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    SetStretchBltMode(dc, HALFTONE);
    GdiAlphaBlend(dc, 0, 0, S, S, src, 0, 0, o.bmWidth, o.bmHeight < 0 ? -o.bmHeight : o.bmHeight, bf);
    SelectObject(src, old);
    DeleteDC(src);
}

/* the window's icon as a LAG_WINICON; 1: not known yet (a Store app's frame without its app), asked again later */
static int send_icon(HWND h, uint32_t id) {
    HICON ic = NULL;
    HBITMAP bm = NULL;
    DWORD_PTR r = 0;
    DWORD pid = 0;
    char app[300];
    wchar_t aumid[AUMID_MAX];
    GetWindowThreadProcessId(h, &pid);
    app_name(pid, app, sizeof app);
    if (!_stricmp(app, "ApplicationFrameHost.exe")) {
        /* a Store app: the frame's own icon is the host's; the app's comes from its id */
        if (hosted_aumid(h, pid, aumid) || !(bm = aumid_icon(aumid, 32))) return pending_young(h);
    } else {
        if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &r) && r) ic = (HICON)r;
        if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &r) && r) ic = (HICON)r;
        if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICON);
        if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
        if (!ic) return 0;
    }
    /* drawn on black and on white: alpha from the difference, so icons with only a mask come out right too */
    enum { S = 32 };
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    uint8_t *pb = NULL, *pw = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pb, NULL, 0), bw = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pw, NULL, 0);
    if (dc && bb && bw) {
        memset(pb, 0, S * S * 4);
        memset(pw, 0xff, S * S * 4);
        HGDIOBJ old = SelectObject(dc, bb);
        draw_icon(dc, ic, bm, S);
        SelectObject(dc, bw);
        draw_icon(dc, ic, bm, S);
        SelectObject(dc, old);
        GdiFlush();
        Buf b = { 0 };
        b32(&b, id); b16(&b, S); b16(&b, S);
        for (int i = 0; i < S * S; i++) {
            int a = 255 - ((pw[i * 4 + 1] - pb[i * 4 + 1]) & 0xff);
            if (pw[i * 4 + 1] < pb[i * 4 + 1]) a = 255;
            uint8_t px[4] = { pb[i * 4 + 2], pb[i * 4 + 1], pb[i * 4], (uint8_t)a };
            if (a > 0 && a < 255) for (int k = 0; k < 3; k++) px[k] = (uint8_t)(px[k] * 255 / a > 255 ? 255 : px[k] * 255 / a);
            bput(&b, px, 4);
        }
        if (!b.oom) send_frame(LAG_WINICON, b.p, (uint32_t)b.n);
        free(b.p);
    }
    if (bb) DeleteObject(bb);
    if (bw) DeleteObject(bw);
    if (dc) DeleteDC(dc);
    if (bm) DeleteObject(bm);
    return 0;
}

/* icons: once per listed window (a Store app's frame again until its app is known); the ones gone are forgotten */
static int icons_left;
static void send_icons(void) {
    uint32_t keep[256];
    int kn = 0;
    for (int i = 0; i < iconed_n; i++) for (int j = 0; j < listed_n; j++) if (iconed[i] == listed[j]) { keep[kn++] = iconed[i]; break; }
    memcpy(iconed, keep, sizeof keep);
    iconed_n = kn;
    icons_left = 0;
    for (int j = 0; j < listed_n; j++) {
        int seen = 0;
        for (int i = 0; i < iconed_n; i++) if (iconed[i] == listed[j]) seen = 1;
        if (seen || iconed_n >= 256) continue;
        if (send_icon((HWND)(LONG_PTR)(LONG)listed[j], listed[j])) icons_left = 1;
        else iconed[iconed_n++] = listed[j];
    }
}

static void send_windows(int force) {
    Buf b = { 0 };
    ListCtx c;
    memset(&c, 0, sizeof c);
    c.b = &b;
    c.fg = GetForegroundWindow();
    mons_stale = 1;                     /* the shown display's corner as it is now, never one from before a change */
    rescue_all("apps", 1);
    shown_rect(&c.desk, NULL);
    apps_note(&c.desk);
    pending_left = 0;
    EnumWindows(list_one, (LPARAM)&c);
    if (pending_left) apps_dirty = 1;           /* a Store app's frame waits for its app: looked at again */
    if (b.oom) { free(b.p); return; }
    if (!force && b.n == last_list.n && (!b.n || !memcmp(b.p, last_list.p, b.n))) {
        free(b.p);
        if (icons_left) { send_icons(); if (icons_left) apps_dirty = 1; }
        return;
    }
    free(last_list.p);
    last_list = b;
    Buf f = { 0 };
    b32(&f, ++win_serial);
    b16(&f, (uint16_t)c.n);
    bput(&f, b.p, b.n);
    if (!f.oom) send_frame(LAG_WINDOWS, f.p, (uint32_t)f.n);
    free(f.p);
    memcpy(listed, c.ids, sizeof listed);
    listed_n = c.n;
    send_icons();
    if (icons_left) apps_dirty = 1;
}

static void CALLBACK on_win_event(HWINEVENTHOOK hk, DWORD ev, HWND h, LONG obj, LONG child, DWORD th, DWORD t) {
    (void)hk; (void)th; (void)t; (void)ev;
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    if (GetAncestor(h, GA_ROOT) != h) return;
    apps_dirty = 1;
}

static int app_cmp(const void *a, const void *b) { return _stricmp(((const App *)a)->name, ((const App *)b)->name); }
static void apps_free(void) {
    for (int i = 0; i < apps_n; i++) { free(apps[i].name); free(apps[i].token); }
    free(apps);
    apps = NULL;
    apps_n = 0;
}

/* the Start menu's own list (shell:AppsFolder: desktop programs and Store apps alike); a token is
 * "shell:AppsFolder\<the item's parsing name>" */
static void send_applist(void) {
    apps_free();
    apps = calloc(LAG_APPS_MAX, sizeof *apps);
    IShellItem *folder = NULL;
    IEnumShellItems *en = NULL;
    if (apps && SUCCEEDED(SHGetKnownFolderItem(&FOLDERID_AppsFolder, 0, NULL, &IID_IShellItem, (void **)&folder)) &&
        SUCCEEDED(IShellItem_BindToHandler(folder, NULL, &BHID_EnumItems, &IID_IEnumShellItems, (void **)&en))) {
        IShellItem *it;
        while (apps_n < LAG_APPS_MAX && IEnumShellItems_Next(en, 1, &it, NULL) == S_OK) {
            wchar_t *name = NULL, *parse = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_NORMALDISPLAY, &name)) && SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_PARENTRELATIVEPARSING, &parse))) {
                size_t tl = wcslen(parse) + 20;
                wchar_t *tok = malloc(tl * sizeof *tok);
                if (tok) {
                    _snwprintf(tok, tl, L"shell:AppsFolder\\%ls", parse);
                    tok[tl - 1] = 0;
                    char *n8 = to_utf8(name, -1, NULL), *t8 = to_utf8(tok, -1, NULL);
                    if (n8 && t8 && *n8 && strlen(n8) <= LAG_NAME_MAX && strlen(t8) <= LAG_TOKEN_MAX) { apps[apps_n].name = n8; apps[apps_n].token = t8; apps_n++; }
                    else { free(n8); free(t8); }
                    free(tok);
                }
            }
            if (name) CoTaskMemFree(name);
            if (parse) CoTaskMemFree(parse);
            IShellItem_Release(it);
        }
    }
    if (en) IEnumShellItems_Release(en);
    if (folder) IShellItem_Release(folder);
    qsort(apps, (size_t)apps_n, sizeof *apps, app_cmp);
    Buf b = { 0 };
    b16(&b, (uint16_t)apps_n);
    for (int i = 0; i < apps_n; i++) { bstr(&b, apps[i].name, LAG_NAME_MAX); bstr(&b, apps[i].token, LAG_TOKEN_MAX); }
    if (!b.oom) send_frame(LAG_APPLIST, b.p, (uint32_t)b.n);
    free(b.p);
    lg("apps: %d programs listed", apps_n);
}

static void taskbar(int hide) {
    APPBARDATA ab;
    memset(&ab, 0, sizeof ab);
    ab.cbSize = sizeof ab;
    ab.hWnd = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!ab.hWnd) return;
    if (hide) { taskbar_was = (int)SHAppBarMessage(ABM_GETSTATE, &ab); ab.lParam = ABS_AUTOHIDE; }
    else ab.lParam = taskbar_was;
    SHAppBarMessage(ABM_SETSTATE, &ab);
}

static void apps_start(void) {
    if (!apps_on) {
        apps_on = 1;
        taskbar(1);
        hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, NULL, on_win_event, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        hooks[1] = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_UNCLOAKED, NULL, on_win_event, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(win, T_APPS, 33, NULL);
        iconed_n = 0;
        rescued_n = 0;
        mons_stale = 1;
        lg("apps mode on");
    }
    send_applist();
    send_windows(1);
}

static void apps_stop(void) {
    if (!apps_on) return;
    apps_on = 0;
    for (int i = 0; i < 2; i++) if (hooks[i]) { UnhookWinEvent(hooks[i]); hooks[i] = NULL; }
    KillTimer(win, T_APPS);
    taskbar(0);
    /* nothing stays on a display the picture doesn't show */
    mons_stale = 1;
    rescue_all("apps mode off", 0);
    memset(&apps_said, 0, sizeof apps_said);
    free(last_list.p);
    memset(&last_list, 0, sizeof last_list);
    listed_n = 0;
    lg("apps mode off");
}

static void focus(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), them = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (them && them != me) AttachThreadInput(me, them, TRUE);
    BOOL ok = SetForegroundWindow(h);
    BringWindowToTop(h);
    if (them && them != me) AttachThreadInput(me, them, FALSE);
    if (!ok) {
        /* a key event of this process lets it take the foreground */
        INPUT in[2];
        memset(in, 0, sizeof in);
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof *in);
        SetForegroundWindow(h);
    }
}

static void win_act(const LagAct *a) {
    int known = 0;
    for (int i = 0; i < listed_n; i++) if (listed[i] == a->id) known = 1;
    HWND h = (HWND)(LONG_PTR)(LONG)a->id;
    if (!known || !IsWindow(h)) { lg("apps: action %d for a window not listed", a->action); return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    int elev = elevated_pid(pid);
    switch (a->action) {
    case LAGA_FOCUS: focus(h); break;
    case LAGA_CLOSE:
        /* tried even when it looks elevated (a program may take WM_CLOSE through its message filter); a refusal is said */
        if (!PostMessageW(h, WM_CLOSE, 0, 0)) {
            DWORD e = GetLastError();
            win_refused(a->id, LAGA_CLOSE, elev ? LAGR_ELEVATED : LAGR_DENIED);
            if (!elev && e != ERROR_ACCESS_DENIED) lg("apps: closing window %lx failed (error %lu)", (unsigned long)a->id, (unsigned long)e);
        }
        break;
    case LAGA_MINIMIZE:
    case LAGA_RESTORE:
        if (elev) { win_refused(a->id, a->action, LAGR_ELEVATED); break; }
        ShowWindowAsync(h, a->action == LAGA_MINIMIZE ? SW_MINIMIZE : SW_RESTORE);
        break;
    case LAGA_MOVE: {
        if (elev) { win_refused(a->id, LAGA_MOVE, LAGR_ELEVATED); break; }
        /* the host's coordinates are the shown display's (its 0,0 is that display's corner); never outside it: a window
         * put off it is out of the picture or dropped from the list (the host clamps too; this side keeps it so with an
         * older host, apps mode on several host displays) */
        RECT m, t, want;
        shown_rect(&m, NULL);
        apps_note(&m);
        SetRect(&want, m.left + a->x, m.top + a->y, m.left + a->x + a->w, m.top + a->y + a->h);
        t = want;
        clamp_into(&t, &m);
        if (!EqualRect(&t, &want))
            lg("apps: a move to %d,%d (%dx%d) kept on the shown display at %ld,%ld (%ldx%ld)", a->x, a->y, a->w, a->h, t.left - m.left,
               t.top - m.top, t.right - t.left, t.bottom - t.top);
        place_frame(h, &t);
        break;
    }
    }
    apps_dirty = 1;
}

static void launch(const uint8_t *tok, uint16_t n) {
    for (int i = 0; i < apps_n; i++) {
        if (strlen(apps[i].token) != n || memcmp(apps[i].token, tok, n)) continue;
        wchar_t *w = to_utf16(tok, n);
        if (!w) return;
        HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        lg("apps: started %s (%d)", apps[i].name, (int)(INT_PTR)r);
        free(w);
        return;
    }
    lg("apps: a launch token not in the list: ignored");
}

/* ---- opening a file the host just sent (AGK_OPEN), or handing files dropped on a window to it (AGK_PLACE) ---- */
typedef struct { char tok[AG_OPEN_TOKEN + 1]; uint64_t size; uint8_t sha[32]; wchar_t *path, *name; uint8_t status; DWORD code; int place; AgPlace pl; } OpenJob;
static volatile LONG open_busy;     /* jobs on their way (each in its own thread: a consent prompt mustn't stop the rest) */
#define OPEN_MAX 12

static void open_reply(const OpenJob *j, uint8_t st, uint8_t how, DWORD code) {
    if (j->place) {
        uint8_t r[AG_PLACED_BYTES];
        if (!ag_placed_write(r, j->tok, st, st == AGOS_OK ? how : AGP_NONE, (uint32_t)code)) send_frame(AGK_PLACED, r, sizeof r);
    } else {
        uint8_t r[AG_OPENED_BYTES];
        if (!ag_opened_write(r, j->tok, st, (uint32_t)code)) send_frame(AGK_OPENED, r, sizeof r);
    }
}

/* SHA-256 of what h reads from its start (the system's own BCrypt); 0 ok */
static int sha256_handle(HANDLE h, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    int ok = 0;
    uint8_t *buf = malloc(1 << 20);
    if (!buf) return -1;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) { free(buf); return -1; }
    if (BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) >= 0) {
        ok = 1;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf, 1 << 20, &got, NULL)) { ok = 0; break; }
            if (!got) break;
            if (BCryptHashData(hh, buf, got, 0) < 0) { ok = 0; break; }
        }
        if (ok && BCryptFinishHash(hh, out, 32, 0) < 0) ok = 0;
        BCryptDestroyHash(hh);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    return ok ? 0 : -1;
}

/* the file of a job, checked again: a regular file (not a directory, link or reparse point) of the size and SHA-256
 * the host measured. AGOS_OK or AGOS_CHANGED (j->code: the system's error, if one) */
static uint8_t verify_file(OpenJob *j) {
    HANDLE h = CreateFileW(j->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) { j->code = GetLastError(); return AGOS_CHANGED; }
    BY_HANDLE_FILE_INFORMATION fi;
    uint8_t d[32], st = AGOS_CHANGED;
    if (GetFileInformationByHandle(h, &fi) && !(fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
        ((uint64_t)fi.nFileSizeHigh << 32 | fi.nFileSizeLow) == j->size && !sha256_handle(h, d) && !memcmp(d, j->sha, 32))
        st = AGOS_OK;
    CloseHandle(h);
    return st;
}

/* the job's thread: the file checked, then opened (AGK_OPEN) or only checked (AGK_PLACE: the batch is handed over on
 * the main thread once all of it is there) */
static DWORD WINAPI open_main(LPVOID arg) {
    OpenJob *j = arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    j->status = verify_file(j);
    if (j->status == AGOS_OK && !j->place) {
        /* its own folder as the working folder, as a double click in Explorer gives */
        wchar_t dir[MAX_PATH * 2];
        wcsncpy(dir, j->path, MAX_PATH * 2 - 1);
        dir[MAX_PATH * 2 - 1] = 0;
        wchar_t *sl = wcsrchr(dir, L'\\');
        if (sl) *sl = 0;
        AllowSetForegroundWindow(ASFW_ANY);
        SHELLEXECUTEINFOW e;
        memset(&e, 0, sizeof e);
        e.cbSize = sizeof e;
        e.fMask = SEE_MASK_NOASYNC;
        e.lpVerb = L"open";
        e.lpFile = j->path;
        e.lpDirectory = sl ? dir : NULL;
        e.nShow = SW_SHOWNORMAL;
        BOOL ok = ShellExecuteExW(&e);
        DWORD err = ok ? 0 : GetLastError();
        /* a type nothing opens: the system's own "How do you want to open this file?" */
        if (!ok && (err == ERROR_NO_ASSOCIATION || err == SE_ERR_NOASSOC)) { e.lpVerb = L"openas"; ok = ShellExecuteExW(&e); err = ok ? 0 : GetLastError(); }
        if (!ok) { j->status = AGOS_FAILED; j->code = err ? err : ERROR_GEN_FAILURE; }
    }
    CoUninitialize();
    if (!PostMessageW(win, WM_APP_OPENED, 0, (LPARAM)j)) { free(j->name); free(j->path); free(j); InterlockedDecrement(&open_busy); }
    return 0;
}

/* the record of a token (see the top): read once and removed; "path|name" in base64, a full path of the person's (drive
 * or network) and the drop's own name (no folder part), text without control characters, at most AG_OPEN_RECORD_MS
 * old. The path (malloc'd; *name too, or NULL when the record has none), or NULL */
static wchar_t *read_record(const char *tok, wchar_t **name) {
    *name = NULL;
    wchar_t *la = NULL, rec[MAX_PATH];
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la))) return NULL;
    _snwprintf(rec, MAX_PATH, L"%ls\\LordOS\\open\\%hs", la, tok);
    rec[MAX_PATH - 1] = 0;
    CoTaskMemFree(la);
    char b64[4096];
    DWORD got = 0;
    int good = 0;
    HANDLE h = CreateFileW(rec, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION fi;
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        uint64_t tn = (uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime;
        if (GetFileInformationByHandle(h, &fi) && !(fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            !fi.nFileSizeHigh && fi.nFileSizeLow > 0 && fi.nFileSizeLow < sizeof b64) {
            uint64_t t0 = (uint64_t)fi.ftLastWriteTime.dwHighDateTime << 32 | fi.ftLastWriteTime.dwLowDateTime;
            /* FILETIME counts 100 ns */
            good = tn >= t0 && (tn - t0) / 10000 <= AG_OPEN_RECORD_MS && ReadFile(h, b64, fi.nFileSizeLow, &got, NULL) && got == fi.nFileSizeLow;
        }
        CloseHandle(h);
        DeleteFileW(rec);
    }
    uint8_t path8[2048], name8[1024];
    const char *bar = good ? memchr(b64, '|', got) : NULL;
    long pn = good ? ag_b64_decode(b64, bar ? (size_t)(bar - b64) : got, path8, sizeof path8 - 1) : -1;
    if (pn < 4 || !lag_text_ok(path8, (size_t)pn) || !((path8[1] == ':' && path8[2] == '\\') || (path8[0] == '\\' && path8[1] == '\\'))) return NULL;
    long nn = bar ? ag_b64_decode(bar + 1, got - (size_t)(bar + 1 - b64), name8, sizeof name8 - 1) : -1;
    if (nn > 0 && lag_text_ok(name8, (size_t)nn) && !memchr(name8, '\\', (size_t)nn) && !memchr(name8, '/', (size_t)nn) && !memchr(name8, ':', (size_t)nn) &&
        !(nn <= 2 && name8[0] == '.' && (nn == 1 || name8[1] == '.'))) *name = to_utf16(name8, (size_t)nn);
    return to_utf16(path8, (size_t)pn);
}

/* AGK_OPEN / AGK_PLACE: the record of that token, then the rest in a thread */
static void open_begin(const char *tok, uint64_t size, const uint8_t sha[32], const AgPlace *pl) {
    OpenJob *j = calloc(1, sizeof *j);
    if (!j) return;
    memcpy(j->tok, tok, sizeof j->tok);
    j->size = size;
    memcpy(j->sha, sha, 32);
    if (pl) { j->place = 1; j->pl = *pl; }
    j->path = read_record(tok, &j->name);
    if (!j->path) { lg("open: no usable record for %.8s…", tok); open_reply(j, AGOS_NORECORD, 0, 0); free(j); return; }
    if (InterlockedIncrement(&open_busy) > OPEN_MAX) { InterlockedDecrement(&open_busy); open_reply(j, AGOS_FAILED, 0, ERROR_BUSY); free(j->name); free(j->path); free(j); return; }
    HANDLE th = CreateThread(NULL, 0, open_main, j, 0, NULL);
    if (!th) { InterlockedDecrement(&open_busy); open_reply(j, AGOS_FAILED, 0, GetLastError()); free(j->name); free(j->path); free(j); return; }
    CloseHandle(th);
}

/* ---- files dropped on an apps mode window (AGK_PLACE): gathered per batch, then handed to that window ---- */
typedef struct { uint32_t batch, win; int32_t x, y; uint8_t count, got; DWORD at; OpenJob *f[AG_PLACE_MAX]; } Batch;
static Batch batches[4];

/* an Explorer window's folder (IShellWindows: the window whose HWND is root, its view's folder), or 0 */
static int explorer_folder(HWND root, wchar_t out[MAX_PATH]) {
    wchar_t cls[64];
    if (!GetClassNameW(root, cls, 64) || wcscmp(cls, L"CabinetWClass")) return 0;
    IShellWindows *sw = NULL;
    int found = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_ALL, &IID_IShellWindows, (void **)&sw))) return 0;
    long n = 0;
    IShellWindows_get_Count(sw, &n);
    for (long i = 0; i < n && !found; i++) {
        VARIANT v;
        VariantInit(&v);
        V_VT(&v) = VT_I4;
        V_I4(&v) = i;
        IDispatch *d = NULL;
        if (FAILED(IShellWindows_Item(sw, v, &d)) || !d) continue;
        IWebBrowserApp *wb = NULL;
        if (SUCCEEDED(IDispatch_QueryInterface(d, &IID_IWebBrowserApp, (void **)&wb))) {
            SHANDLE_PTR hw = 0;
            IServiceProvider *sp = NULL;
            IShellBrowser *sb = NULL;
            IShellView *sv = NULL;
            IFolderView *fv = NULL;
            IPersistFolder2 *pf = NULL;
            if (SUCCEEDED(IWebBrowserApp_get_HWND(wb, &hw)) && (HWND)hw == root &&
                SUCCEEDED(IWebBrowserApp_QueryInterface(wb, &IID_IServiceProvider, (void **)&sp)) &&
                SUCCEEDED(IServiceProvider_QueryService(sp, &SID_STopLevelBrowser, &IID_IShellBrowser, (void **)&sb)) &&
                SUCCEEDED(IShellBrowser_QueryActiveShellView(sb, &sv)) &&
                SUCCEEDED(IShellView_QueryInterface(sv, &IID_IFolderView, (void **)&fv)) &&
                SUCCEEDED(IFolderView_GetFolder(fv, &IID_IPersistFolder2, (void **)&pf))) {
                PIDLIST_ABSOLUTE pidl = NULL;
                if (SUCCEEDED(IPersistFolder2_GetCurFolder(pf, &pidl)) && pidl) { found = SHGetPathFromIDListW(pidl, out) && out[0]; CoTaskMemFree(pidl); }
            }
            if (pf) IPersistFolder2_Release(pf);
            if (fv) IFolderView_Release(fv);
            if (sv) IShellView_Release(sv);
            if (sb) IShellBrowser_Release(sb);
            if (sp) IServiceProvider_Release(sp);
            IWebBrowserApp_Release(wb);
        }
        IDispatch_Release(d);
    }
    IShellWindows_Release(sw);
    if (found) {
        DWORD at = GetFileAttributesW(out);
        found = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && !(at & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    return found;
}

/* the paths as a DROPFILES block (CF_HDROP, WM_DROPFILES), pt in the target's client coordinates */
static HGLOBAL hdrop_of(OpenJob **f, int n, POINT pt) {
    size_t chars = 1;
    for (int i = 0; i < n; i++) chars += wcslen(f[i]->path) + 1;
    HGLOBAL g = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    DROPFILES *df = g ? GlobalLock(g) : NULL;
    if (!df) { if (g) GlobalFree(g); return NULL; }
    df->pFiles = sizeof(DROPFILES);
    df->pt = pt;
    df->fNC = FALSE;
    df->fWide = TRUE;
    wchar_t *w = (wchar_t *)((uint8_t *)df + sizeof(DROPFILES));
    for (int i = 0; i < n; i++) { size_t l = wcslen(f[i]->path) + 1; memcpy(w, f[i]->path, l * sizeof(wchar_t)); w += l; }
    *w = 0;
    GlobalUnlock(g);
    return g;
}

/* a free name for name in folder ("name (2).ext"…); 0 ok */
static int free_name(const wchar_t *folder, const wchar_t *name, wchar_t out[MAX_PATH * 2]) {
    const wchar_t *dot = wcsrchr(name, L'.');
    size_t bl = dot && dot != name ? (size_t)(dot - name) : wcslen(name);
    for (int i = 1; i < 1000; i++) {
        if (i == 1) _snwprintf(out, MAX_PATH * 2, L"%ls\\%ls", folder, name);
        else _snwprintf(out, MAX_PATH * 2, L"%ls\\%.*ls (%d)%ls", folder, (int)bl, name, i, dot && dot != name ? dot : L"");
        out[MAX_PATH * 2 - 1] = 0;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
    }
    return -1;
}

/* a whole batch (or what came of it): handed to the window it was dropped on, as the person. Only these files, which
 * arrived through this drop and were checked: an Explorer folder window gets them moved into its folder, a window that
 * takes dropped files gets WM_DROPFILES, any other the clipboard (CF_HDROP, "copy"), to paste */
static void batch_deliver(Batch *b) {
    OpenJob *ok[AG_PLACE_MAX];
    int n = 0;
    for (int i = 0; i < b->count; i++) if (b->f[i] && b->f[i]->status == AGOS_OK) ok[n++] = b->f[i];
    HWND root = (HWND)(LONG_PTR)(LONG)b->win;
    int known = 0;
    for (int i = 0; i < listed_n; i++) known |= listed[i] == b->win;
    uint8_t how = AGP_NONE;
    DWORD err = 0;
    if (n && known && IsWindow(root)) {
        wchar_t folder[MAX_PATH];
        RECT m;
        shown_rect(&m, NULL);
        POINT pt = { m.left + b->x, m.top + b->y };       /* the host's point is on the shown display */
        HWND under = WindowFromPoint(pt);
        if (!under || GetAncestor(under, GA_ROOT) != root) under = root;
        if (explorer_folder(root, folder)) {
            int moved = 0;
            for (int i = 0; i < n; i++) {
                /* the name it had on the host (its copy in Downloads may have got " (2)") */
                const wchar_t *nm = wcsrchr(ok[i]->path, L'\\');
                wchar_t to[MAX_PATH * 2];
                if (!nm || free_name(folder, ok[i]->name ? ok[i]->name : nm + 1, to)) continue;
                if (MoveFileExW(ok[i]->path, to, MOVEFILE_COPY_ALLOWED)) { free(ok[i]->path); ok[i]->path = _wcsdup(to); moved++; }
                else err = GetLastError();
            }
            if (moved) { how = AGP_FOLDER; SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, folder, NULL); }
        }
        if (!how) {
            HWND t = under;
            while (t && !(GetWindowLongW(t, GWL_EXSTYLE) & WS_EX_ACCEPTFILES)) t = t == root ? NULL : GetParent(t);
            if (t) {
                POINT cp = pt;
                ScreenToClient(t, &cp);
                HGLOBAL g = hdrop_of(ok, n, cp);
                if (g && PostMessageW(t, WM_DROPFILES, (WPARAM)g, 0)) how = AGP_DROPPED;
                else if (g) { err = GetLastError(); GlobalFree(g); }
            }
        }
        if (!how && open_clip()) {
            POINT z = { 0, 0 };
            HGLOBAL g = hdrop_of(ok, n, z), e = GlobalAlloc(GHND, sizeof(DWORD));
            if (e) { DWORD *v = GlobalLock(e); *v = DROPEFFECT_COPY; GlobalUnlock(e); }
            EmptyClipboard();
            if (g && SetClipboardData(CF_HDROP, g)) { how = AGP_CLIPBOARD; g = NULL; }
            if (e && SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), e)) e = NULL;
            CloseClipboard();
            if (g) GlobalFree(g);
            if (e) GlobalFree(e);
        }
    } else if (n) err = ERROR_INVALID_WINDOW_HANDLE;
    lg("place: %d of %d files of batch %u to window %08x: %s", n, b->count, b->batch, b->win,
       how == AGP_FOLDER ? "into its folder" : how == AGP_DROPPED ? "dropped" : how == AGP_CLIPBOARD ? "on the clipboard" : "not handed over");
    for (int i = 0; i < b->count; i++) {
        OpenJob *j = b->f[i];
        if (!j) continue;
        if (port != INVALID_HANDLE_VALUE && host_hello) {
            if (j->status != AGOS_OK) open_reply(j, j->status, 0, j->code);
            else if (how) open_reply(j, AGOS_OK, how, 0);
            else open_reply(j, AGOS_FAILED, 0, err ? err : ERROR_GEN_FAILURE);
        }
        free(j->name);
        free(j->path);
        free(j);
    }
    memset(b, 0, sizeof *b);
}

/* a checked file of a batch: kept until the batch is all there */
static void batch_add(OpenJob *j) {
    Batch *b = NULL;
    for (int i = 0; i < 4 && !b; i++) if (batches[i].batch == j->pl.batch && batches[i].win == j->pl.win) b = &batches[i];
    for (int i = 0; i < 4 && !b; i++) if (!batches[i].batch) { b = &batches[i]; b->batch = j->pl.batch; b->win = j->pl.win; b->count = j->pl.count; b->at = GetTickCount(); }
    if (!b || j->pl.count != b->count || b->f[j->pl.index]) { open_reply(j, AGOS_FAILED, 0, ERROR_BUSY); free(j->name); free(j->path); free(j); return; }
    b->x = j->pl.x; b->y = j->pl.y;
    b->f[j->pl.index] = j;
    if (++b->got == b->count) batch_deliver(b);
}
/* a batch whose other files never came (the host refused or lost them): what is there goes after 10 minutes */
static void batch_tick(void) {
    for (int i = 0; i < 4; i++) if (batches[i].batch && GetTickCount() - batches[i].at > AG_OPEN_RECORD_MS) batch_deliver(&batches[i]);
}

static void open_done(OpenJob *j) {
    InterlockedDecrement(&open_busy);
    if (j->place) { batch_add(j); return; }
    char *p8 = to_utf8(j->path, -1, NULL);
    if (j->status == AGOS_OK) lg("open: opened %s", p8 ? p8 : "?");
    else lg("open: %s not opened (%s, error %lu)", p8 ? p8 : "?", j->status == AGOS_CHANGED ? "not the file sent" : "the system couldn't open it", (unsigned long)j->code);
    free(p8);
    if (port != INVALID_HANDLE_VALUE && host_hello) open_reply(j, j->status, 0, j->status == AGOS_OK ? 0 : j->code);
    free(j->name);
    free(j->path);
    free(j);
}

/* ---- the performance profile (AGK_PERF, w11; docs/VIRTUALIZATION.md 7.1.4) ----
 * Only what this person may change without elevation, nothing named by the host (one bit: on or off):
 *   - the power plan: High performance (PowerSetActiveScheme, powrprof; a standard user may switch plans unless a
 *     policy says otherwise; when Windows refuses, or the plan isn't there, the part is said refused);
 *   - visual effects: the animations, fades, shadows and window contents while dragging that "Adjust for best
 *     performance" turns off, set for this session only (SystemParametersInfo without SPIF_UPDATEINIFILE: the person's
 *     profile keeps its own values);
 *   - transparency (Personalize's EnableTransparency in the person's own registry).
 * What was there before is kept in HKCU\Software\LordOS\GuestWin PerfSaved (a fixed record, read back checked) and put
 * back when the host says off, when the port is let go (log off, another session at the console, exit) and when these
 * tools start and find a record (a crash): a part goes back only while it is still what this side set, so a change the
 * person made in between stays. Windows Search's indexing isn't paused: its service needs administrator rights, which
 * these tools never have. */
static const GUID PERF_HIGH = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };  /* GUID_MIN_POWER_SAVINGS */
#define PERF_KEY L"Software\\LordOS\\GuestWin"
#define PERF_VAL L"PerfSaved"
#define PERF_PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define PERF_MAGIC 0x3146504cu                  /* "LPF1" */
typedef struct {
    uint32_t magic, size;
    uint32_t parts;                             /* AGPF_* this side changed and puts back */
    uint32_t done;                              /* AGPF_* at their minimum (changed or already so) */
    GUID scheme_was;                            /* the plan before (AGPF_POWER) */
    uint32_t fx_was;                            /* bit i: effect FX[i] was on before and this side turned it off */
    uint32_t transp_was, transp_had;            /* EnableTransparency before, and whether there was one */
} PerfSaved;
static PerfSaved perf;                          /* magic 0: nothing applied */
enum { FX_BOOL, FX_UIPARAM, FX_ANIMINFO };
static const struct { UINT get, set; int how; const char *name; } FX[] = {
    { SPI_GETCLIENTAREAANIMATION, SPI_SETCLIENTAREAANIMATION, FX_BOOL, "animations inside windows" },
    { SPI_GETANIMATION, SPI_SETANIMATION, FX_ANIMINFO, "minimize and maximize animation" },
    { SPI_GETMENUANIMATION, SPI_SETMENUANIMATION, FX_BOOL, "menu animation" },
    { SPI_GETCOMBOBOXANIMATION, SPI_SETCOMBOBOXANIMATION, FX_BOOL, "combo box animation" },
    { SPI_GETLISTBOXSMOOTHSCROLLING, SPI_SETLISTBOXSMOOTHSCROLLING, FX_BOOL, "smooth list scrolling" },
    { SPI_GETTOOLTIPANIMATION, SPI_SETTOOLTIPANIMATION, FX_BOOL, "tooltip animation" },
    { SPI_GETSELECTIONFADE, SPI_SETSELECTIONFADE, FX_BOOL, "selection fade" },
    { SPI_GETCURSORSHADOW, SPI_SETCURSORSHADOW, FX_BOOL, "pointer shadow" },
    { SPI_GETDROPSHADOW, SPI_SETDROPSHADOW, FX_BOOL, "window shadows" },
    { SPI_GETDRAGFULLWINDOWS, SPI_SETDRAGFULLWINDOWS, FX_UIPARAM, "window contents while dragging" },
};
#define FX_N ((int)(sizeof FX / sizeof FX[0]))

/* 1 on, 0 off, -1 the system didn't say */
static int fx_get(int i) {
    if (FX[i].how == FX_ANIMINFO) {
        ANIMATIONINFO ai = { sizeof ai, 0 };
        return SystemParametersInfoW(FX[i].get, sizeof ai, &ai, 0) ? ai.iMinAnimate != 0 : -1;
    }
    BOOL v = FALSE;
    return SystemParametersInfoW(FX[i].get, 0, &v, 0) ? v != 0 : -1;
}
/* this session only: no SPIF_UPDATEINIFILE */
static int fx_set(int i, int on) {
    if (FX[i].how == FX_ANIMINFO) {
        ANIMATIONINFO ai = { sizeof ai, on };
        return SystemParametersInfoW(FX[i].set, sizeof ai, &ai, SPIF_SENDCHANGE) ? 0 : -1;
    }
    if (FX[i].how == FX_UIPARAM) return SystemParametersInfoW(FX[i].set, (UINT)on, NULL, SPIF_SENDCHANGE) ? 0 : -1;
    return SystemParametersInfoW(FX[i].set, 0, (PVOID)(INT_PTR)(on ? TRUE : FALSE), SPIF_SENDCHANGE) ? 0 : -1;
}

/* EnableTransparency: 1 there (*v its value), 0 not there */
static int transp_get(DWORD *v) {
    DWORD n = sizeof *v;
    return RegGetValueW(HKEY_CURRENT_USER, PERF_PERSONALIZE, L"EnableTransparency", RRF_RT_REG_DWORD, NULL, v, &n) == ERROR_SUCCESS;
}
static void transp_changed(void) {
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000, &r);
}

static void perf_save(void) {
    if (perf.magic) RegSetKeyValueW(HKEY_CURRENT_USER, PERF_KEY, PERF_VAL, REG_BINARY, &perf, sizeof perf);
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, PERF_KEY, PERF_VAL);
}

static void perf_say(int on, unsigned done, unsigned failed) {
    uint8_t b[AG_PERFSTATE_BYTES];
    if (port == INVALID_HANDLE_VALUE || !host_hello || ag_perfstate_write(b, on, done, failed)) return;
    send_frame(AGK_PERFSTATE, b, sizeof b);
}

static void perf_apply(void) {
    if (perf.magic) { lg("performance profile: already on"); perf_say(1, perf.done, 0); return; }
    PerfSaved s;
    memset(&s, 0, sizeof s);
    s.magic = PERF_MAGIC;
    s.size = sizeof s;
    unsigned failed = 0;
    /* the power plan */
    GUID *cur = NULL;
    DWORD e = PowerGetActiveScheme(NULL, &cur);
    if (e == ERROR_SUCCESS && cur && IsEqualGUID(cur, &PERF_HIGH)) { s.done |= AGPF_POWER; lg("performance profile: the power plan is High performance already"); }
    else if (e == ERROR_SUCCESS && cur) {
        s.scheme_was = *cur;
        e = PowerSetActiveScheme(NULL, &PERF_HIGH);
        if (e == ERROR_SUCCESS) { s.parts |= AGPF_POWER; s.done |= AGPF_POWER; lg("performance profile: power plan set to High performance"); }
        else { failed |= AGPF_POWER; lg("performance profile: Windows refused the High performance power plan (error %lu)", (unsigned long)e); }
    } else { failed |= AGPF_POWER; lg("performance profile: the power plan can't be read (error %lu)", (unsigned long)e); }
    if (cur) LocalFree(cur);
    /* the visual effects, for this session */
    int fx_bad = 0, fx_n = 0;
    for (int i = 0; i < FX_N; i++) {
        int on = fx_get(i);
        if (on < 0) { fx_bad = 1; continue; }
        if (!on) continue;
        if (fx_set(i, 0)) { fx_bad = 1; lg("performance profile: %s not turned off (error %lu)", FX[i].name, (unsigned long)GetLastError()); continue; }
        s.fx_was |= 1u << i;
        fx_n++;
    }
    if (s.fx_was) s.parts |= AGPF_EFFECTS;
    if (fx_bad) failed |= AGPF_EFFECTS; else s.done |= AGPF_EFFECTS;
    lg("performance profile: %d visual effect%s turned off for this session", fx_n, fx_n == 1 ? "" : "s");
    /* transparency */
    DWORD tv = 1;
    s.transp_had = (uint32_t)transp_get(&tv);
    s.transp_was = tv;
    if (s.transp_had && !tv) s.done |= AGPF_TRANSPARENCY;
    else {
        DWORD z = 0;
        LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, PERF_PERSONALIZE, L"EnableTransparency", REG_DWORD, &z, sizeof z);
        if (r == ERROR_SUCCESS) { s.parts |= AGPF_TRANSPARENCY; s.done |= AGPF_TRANSPARENCY; transp_changed(); lg("performance profile: transparency turned off"); }
        else { failed |= AGPF_TRANSPARENCY; lg("performance profile: transparency not turned off (error %ld)", (long)r); }
    }
    perf = s;
    perf_save();
    perf_say(1, s.done, failed);
}

/* puts back what perf_apply changed, only where it is still what this side set */
static void perf_restore(const char *why, int say) {
    if (!perf.magic) { if (say) perf_say(0, 0, 0); return; }
    unsigned done = 0, failed = 0;
    if (perf.parts & AGPF_POWER) {
        GUID *cur = NULL;
        DWORD e = PowerGetActiveScheme(NULL, &cur);
        if (e == ERROR_SUCCESS && cur && IsEqualGUID(cur, &PERF_HIGH)) {
            e = PowerSetActiveScheme(NULL, &perf.scheme_was);
            if (e == ERROR_SUCCESS) done |= AGPF_POWER;
            else { failed |= AGPF_POWER; lg("performance profile: the earlier power plan wasn't put back (error %lu)", (unsigned long)e); }
        } else done |= AGPF_POWER;              /* changed since: the person's choice stays */
        if (cur) LocalFree(cur);
    }
    if (perf.parts & AGPF_EFFECTS) {
        int bad = 0;
        for (int i = 0; i < FX_N; i++)
            if ((perf.fx_was & (1u << i)) && fx_get(i) == 0 && fx_set(i, 1)) bad = 1;
        if (bad) failed |= AGPF_EFFECTS; else done |= AGPF_EFFECTS;
    }
    if (perf.parts & AGPF_TRANSPARENCY) {
        DWORD tv = 1;
        if (transp_get(&tv) && !tv) {
            LSTATUS r = perf.transp_had ? RegSetKeyValueW(HKEY_CURRENT_USER, PERF_PERSONALIZE, L"EnableTransparency", REG_DWORD, &perf.transp_was, sizeof perf.transp_was)
                                        : RegDeleteKeyValueW(HKEY_CURRENT_USER, PERF_PERSONALIZE, L"EnableTransparency");
            if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) { done |= AGPF_TRANSPARENCY; transp_changed(); }
            else failed |= AGPF_TRANSPARENCY;
        } else done |= AGPF_TRANSPARENCY;
    }
    lg("performance profile: put back (%s)%s", why, failed ? ", some parts refused" : "");
    memset(&perf, 0, sizeof perf);
    perf_save();
    if (say) perf_say(0, done, failed);
}

/* at start: a record left by a run that didn't end (a crash, the process killed) is put back now */
static void perf_recover(void) {
    PerfSaved s;
    DWORD n = sizeof s, type = 0;
    memset(&s, 0, sizeof s);
    LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, PERF_KEY, PERF_VAL, RRF_RT_REG_BINARY, &type, &s, &n);
    if (r == ERROR_FILE_NOT_FOUND) return;
    if (r != ERROR_SUCCESS || n != sizeof s || s.magic != PERF_MAGIC || s.size != sizeof s || (s.parts & ~(uint32_t)AGPF_ALL) ||
        (s.fx_was >> FX_N) || s.transp_had > 1) {
        lg("performance profile: an unreadable record was dropped");
        RegDeleteKeyValueW(HKEY_CURRENT_USER, PERF_KEY, PERF_VAL);
        return;
    }
    s.done &= AGPF_ALL;
    perf = s;
    perf_restore("left by an earlier run", 0);
}

/* ---- the agent's shell (w12; docs/VIRTUALIZATION.md 6.18) ----
 * PowerShell (%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoLogo, a fixed path from the system's
 * own folder; nothing from the host names a program, an argument, a folder or a variable) in a pseudo console, as the
 * person these tools run as, in their profile folder. Never elevated: tools that run elevated open no shell. A reader
 * thread hands what it writes to this window, at most AG_SHELL_WINDOW unacknowledged by the host; what the host types
 * goes into a pipe as big as that window, acknowledged once written. */
typedef HRESULT (WINAPI *CreatePCFn)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT (WINAPI *ResizePCFn)(void *, COORD);
typedef void (WINAPI *ClosePCFn)(void *);
static struct {
    void *pc;
    HANDLE in_w, out_r, proc, reader, wait, room;
    uint32_t id;
    volatile LONG unacked;              /* sent to the host and not acknowledged */
    LONG gen;
    DWORD code;
    int exited;
} sh;
typedef struct { LONG gen; DWORD n; uint8_t d[]; } ShChunk;

static int token_elevated(void) {
    HANDLE t;
    TOKEN_ELEVATION e = { 0 };
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 1;
    BOOL ok = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n);
    CloseHandle(t);
    return !ok || e.TokenIsElevated;
}

static void sh_close_frame(uint32_t id, int status) {
    uint8_t b[8];
    ag_shell_pair_write(b, id, (uint32_t)status);
    send_frame(AGK_SHELL_CLOSE, b, 8);
}

static DWORD WINAPI sh_reader(LPVOID arg) {
    LONG gen = (LONG)(intptr_t)arg;
    HANDLE r = sh.out_r;
    for (;;) {
        while (sh.unacked >= AG_SHELL_WINDOW) if (WaitForSingleObject(sh.room, 1000) == WAIT_FAILED) break;
        DWORD room = AG_SHELL_WINDOW - (DWORD)sh.unacked;
        if (room > AG_SHELL_CHUNK) room = AG_SHELL_CHUNK;
        ShChunk *c = malloc(sizeof *c + room);
        DWORD got = 0;
        if (!c || !ReadFile(r, c->d, room, &got, NULL) || !got) { free(c); break; }
        c->gen = gen;
        c->n = got;
        InterlockedExchangeAdd(&sh.unacked, (LONG)got);
        if (!PostMessageW(win, WM_APP_SHDATA, 0, (LPARAM)c)) free(c);
    }
    PostMessageW(win, WM_APP_SHEOF, (WPARAM)gen, 0);
    return 0;
}

static VOID CALLBACK sh_exited(PVOID arg, BOOLEAN timed) { (void)timed; PostMessageW(win, WM_APP_SHEXIT, (WPARAM)(intptr_t)arg, 0); }

static ClosePCFn pc_close(void) { HMODULE k = GetModuleHandleW(L"kernel32.dll"); return k ? (ClosePCFn)(void *)GetProcAddress(k, "ClosePseudoConsole") : NULL; }

static void shell_end(void) {
    if (!sh.id && !sh.pc) return;
    InterlockedIncrement(&sh.gen);
    if (sh.wait) { UnregisterWaitEx(sh.wait, INVALID_HANDLE_VALUE); sh.wait = NULL; }
    if (sh.proc && !sh.exited) TerminateProcess(sh.proc, 1);
    ClosePCFn cl = pc_close();
    if (sh.pc && cl) cl(sh.pc);          /* the reader's pipe breaks: it ends */
    sh.pc = NULL;
    if (sh.room) SetEvent(sh.room);
    if (sh.reader) { WaitForSingleObject(sh.reader, 3000); CloseHandle(sh.reader); sh.reader = NULL; }
    if (sh.in_w) { CloseHandle(sh.in_w); sh.in_w = NULL; }
    if (sh.out_r) { CloseHandle(sh.out_r); sh.out_r = NULL; }
    if (sh.proc) { CloseHandle(sh.proc); sh.proc = NULL; }
    sh.id = 0;
    sh.unacked = 0;
    sh.exited = 0;
}

static void shell_open(uint32_t id, int cols, int rows) {
    shell_end();
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    CreatePCFn create = k ? (CreatePCFn)(void *)GetProcAddress(k, "CreatePseudoConsole") : NULL;
    if (token_elevated() || !create) { lg("shell: refused (%s)", create ? "these tools run elevated" : "no pseudo console on this Windows"); sh_close_frame(id, AG_SHELL_NOSTART); return; }
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, NULL, AG_SHELL_WINDOW) || !CreatePipe(&out_r, &out_w, NULL, 0)) {
        if (in_r) { CloseHandle(in_r); CloseHandle(in_w); }
        sh_close_frame(id, AG_SHELL_NOSTART);
        return;
    }
    COORD sz = { (SHORT)cols, (SHORT)rows };
    void *pc = NULL;
    if (FAILED(create(sz, in_r, out_w, 0, &pc))) { CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w); sh_close_frame(id, AG_SHELL_NOSTART); return; }
    CloseHandle(in_r);
    CloseHandle(out_w);
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    si.lpAttributeList = malloc(asz);
    wchar_t sys[MAX_PATH], cmd[MAX_PATH + 64], home[MAX_PATH] = L"";
    UINT sl = GetSystemDirectoryW(sys, MAX_PATH);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    wchar_t *prof = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &prof))) { wcsncpy(home, prof, MAX_PATH - 1); home[MAX_PATH - 1] = 0; CoTaskMemFree(prof); }
    int ok = si.lpAttributeList && sl && sl < MAX_PATH &&
             InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz) &&
             UpdateProcThreadAttribute(si.lpAttributeList, 0, 0x00020016 /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */, pc, sizeof pc, NULL, NULL);
    if (ok) {
        _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo", sys);
        cmd[sizeof cmd / sizeof *cmd - 1] = 0;
        ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL,
                            home[0] ? home : NULL, &si.StartupInfo, &pi);
    }
    if (si.lpAttributeList) { DeleteProcThreadAttributeList(si.lpAttributeList); free(si.lpAttributeList); }
    if (!ok) {
        lg("shell: PowerShell didn't start (%lu)", (unsigned long)GetLastError());
        ClosePCFn cl = pc_close();
        if (cl) cl(pc);
        CloseHandle(in_w); CloseHandle(out_r);
        sh_close_frame(id, AG_SHELL_NOSTART);
        return;
    }
    CloseHandle(pi.hThread);
    sh.pc = pc; sh.in_w = in_w; sh.out_r = out_r; sh.proc = pi.hProcess; sh.id = id; sh.unacked = 0; sh.exited = 0;
    if (!sh.room) sh.room = CreateEventW(NULL, FALSE, FALSE, NULL);
    LONG gen = InterlockedIncrement(&sh.gen);
    sh.reader = CreateThread(NULL, 0, sh_reader, (LPVOID)(intptr_t)gen, 0, NULL);
    RegisterWaitForSingleObject(&sh.wait, pi.hProcess, sh_exited, (PVOID)(intptr_t)gen, INFINITE, WT_EXECUTEONLYONCE);
    lg("shell: PowerShell for the host (%dx%d)", cols, rows);
}

static void shell_frame(const Frame *f) {
    uint32_t id;
    int c, r;
    if (f->kind == AGK_SHELL_OPEN) { if (!ag_shell_size_parse(f->data, f->len, &id, &c, &r)) shell_open(id, c, r); return; }
    if (f->kind == AGK_SHELL_SIZE) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        ResizePCFn rs = k ? (ResizePCFn)(void *)GetProcAddress(k, "ResizePseudoConsole") : NULL;
        COORD sz;
        if (!ag_shell_size_parse(f->data, f->len, &id, &c, &r) && id == sh.id && sh.pc && rs) { sz.X = (SHORT)c; sz.Y = (SHORT)r; rs(sh.pc, sz); }
        return;
    }
    if (f->kind == AGK_SHELL_CLOSE) { int st; if (!ag_shell_close_parse(f->data, f->len, &id, &st) && id == sh.id) shell_end(); return; }
    if (f->kind == AGK_SHELL_ACK) {
        uint32_t by;
        if (ag_shell_ack_parse(f->data, f->len, &id, &by) || id != sh.id) return;
        LONG now = InterlockedExchangeAdd(&sh.unacked, -(LONG)by) - (LONG)by;
        if (now < 0) InterlockedExchange(&sh.unacked, 0);
        SetEvent(sh.room);
        return;
    }
    const uint8_t *d;
    uint32_t dn;
    if (ag_shell_data_parse(f->data, f->len, &id, &d, &dn) || id != sh.id || !sh.in_w) return;
    DWORD w = 0;
    if (WriteFile(sh.in_w, d, dn, &w, NULL) && w) {
        uint8_t b[8];
        ag_shell_pair_write(b, id, w);
        send_frame(AGK_SHELL_ACK, b, 8);
    }
}

/* ---- links and notifications for the host (w12; 6.12) ----
 * `lordos-guest-win.exe --open-on-host URL` or `--notify-host TITLE [BODY]`: run by a program of the person's, it hands
 * the running tools of this session one frame by WM_COPYDATA (dwData: AGK_LINK or AGK_NOTIFY), parsed here as lord-vm
 * will, sent on only while the host's hello said it takes that kind. The exit status is a GSA-like code: 0 sent,
 * 1 not connected, 2 malformed, 3 the machine doesn't take them */
static LRESULT copydata(const COPYDATASTRUCT *cd) {
    if (!cd || (cd->dwData != AGK_LINK && cd->dwData != AGK_NOTIFY) || !cd->lpData || cd->cbData > AG_NOTE_BYTES + AG_LINK_MAX) return 2;
    const uint8_t *p = cd->lpData;
    int link = cd->dwData == AGK_LINK;
    if (link ? !ag_link_ok(p, cd->cbData) : ag_note_parse(p, cd->cbData, NULL, NULL, NULL, NULL)) return 2;
    if (port == INVALID_HANDLE_VALUE || !host_hello) return 1;
    if (!(link ? host_links : host_notify)) return 3;
    return send_frame((uint16_t)cd->dwData, p, cd->cbData) ? 1 : 0;
}

/* UTF-16 to UTF-8, control characters as spaces, at most max bytes, cut on a character */
static size_t note_utf8(const wchar_t *w, char *out, size_t max) {
    char tmp[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, sizeof tmp, NULL, NULL);
    if (n <= 0) { out[0] = 0; return 0; }
    size_t l = strlen(tmp), o = 0;
    for (size_t i = 0; i < l && o < max; i++) out[o++] = ((unsigned char)tmp[i] < 0x20 || tmp[i] == 0x7f) ? ' ' : tmp[i];
    while (o && ((unsigned char)out[o - 1] & 0xc0) == 0x80) o--;
    if (o && ((unsigned char)out[o - 1] & 0xc0) == 0xc0) o--;
    out[o] = 0;
    return o;
}

static int send_to_tools(const wchar_t *what, const wchar_t *a, const wchar_t *b) {
    static uint8_t payload[AG_NOTE_BYTES + AG_LINK_MAX * 3 + 1];
    size_t n = 0;
    ULONG_PTR kind;
    if (!wcscmp(what, L"--open-on-host")) {
        char raw[AG_LINK_MAX * 2];
        int rn = WideCharToMultiByte(CP_UTF8, 0, a, -1, raw, sizeof raw, NULL, NULL);
        long l = rn > 1 ? ag_link_encode((const uint8_t *)raw, (size_t)rn - 1, (char *)payload, sizeof payload) : -1;
        if (l <= 0 || !ag_link_ok(payload, (size_t)l)) return 2;
        n = (size_t)l;
        kind = AGK_LINK;
    } else {
        char t[AG_NOTE_TITLE_MAX + 1], bo[AG_NOTE_BODY_MAX + 1];
        size_t tn = note_utf8(a, t, AG_NOTE_TITLE_MAX), bn = b ? note_utf8(b, bo, AG_NOTE_BODY_MAX) : 0;
        if (!b) bo[0] = 0;
        n = ag_note_write((const uint8_t *)t, tn, (const uint8_t *)bo, bn, payload, sizeof payload);
        if (!n) return 2;
        kind = AGK_NOTIFY;
    }
    HWND to = FindWindowW(L"LordOSGuestWin", L"LordOS guest tools");
    if (!to) return 1;
    COPYDATASTRUCT cd = { kind, (DWORD)n, payload };
    DWORD_PTR res = 1;
    if (!SendMessageTimeoutW(to, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &res)) return 1;
    return (int)res;
}

/* ---- frames from the host ---- */
static void on_frame(const Frame *f) {
    switch (f->kind) {
    case AGK_SHELL_OPEN: case AGK_SHELL_SIZE: case AGK_SHELL_DATA: case AGK_SHELL_CLOSE: case AGK_SHELL_ACK:
        if (host_hello) shell_frame(f);
        break;
    case AGK_HELLO:
        if (f->len < 200) {
            char t[201];
            memcpy(t, f->data, f->len);
            t[f->len] = 0;
            host_hello = 1;
            host_clipx = strstr(t, " clipx") != NULL;
            host_elev = strstr(t, " elevated") != NULL;
            unsigned hc = ag_hello_caps(f->data, f->len);
            host_links = (hc & AG_HELLO_LINKS) != 0;
            host_notify = (hc & AG_HELLO_NOTIFY) != 0;
            if (strstr(t, " ask")) say_hello();
            lg("host: %s", ag_utf8_ok(f->data, f->len) ? t : "(hello)");
            /* the displays, for the host's log (an older lord-vm skips the kind), and how the tablet maps */
            mons_sent = 0;
            mons_stale = 1;
            mon_get();
            tablet_sent = 0;
            tablet_say();
        }
        break;
    case AGK_CLIP:
        if (f->len && f->len <= AG_TEXT_MAX && ag_utf8_ok(f->data, f->len)) { AgClip c; memset(&c, 0, sizeof c); c.text = f->data; c.text_n = f->len; clip_put(&c); }
        break;
    case AGK_CLIPX: {
        AgClip c, k;
        if (ag_clipx_parse(f->data, f->len, &c)) break;
        memset(&k, 0, sizeof k);
        if (c.text && ag_utf8_ok(c.text, c.text_n)) { k.text = c.text; k.text_n = c.text_n; }
        if (c.html && ag_utf8_ok(c.html, c.html_n)) { k.html = c.html; k.html_n = c.html_n; }
        if (c.png && !ag_png_check(c.png, c.png_n, NULL, NULL)) { k.png = c.png; k.png_n = c.png_n; }
        if (k.text || k.html || k.png) clip_put(&k);
        break;
    }
    case AGK_CLIPOFFER: {
        uint32_t gen;
        uint8_t forms, flags;
        if (!ag_offer_parse(f->data, f->len, &gen, &forms, &flags)) clip_offer(gen, forms, flags);
        break;
    }
    case AGK_DISPLAY: {
        uint32_t w, h;
        if (ag_display_parse(f->data, f->len, &w, &h)) break;
        disp_w = w; disp_h = h;
        disp_tries = 0;
        disp_got_w = disp_got_h = 0;
        disp_again = 0;
        disp_asked_at = GetTickCount64();
        mons_stale = 1;
        lg("display: the host wants %ux%u", w, h);
        SetTimer(win, T_DISP, 250, NULL);
        disp_tick();
        break;
    }
    case AGK_EXTRA: {
        int off;
        if (!host_hello || ag_extra_parse(f->data, f->len, &off)) break;
        if (off ? !extra_off() : !extra_on("the host asked")) { mons_stale = 1; SetTimer(win, T_MONS, 1500, NULL); }
        say_hello();                    /* "extraoff" or not: Machines offers the other button */
        break;
    }
    case AGK_PERF: {
        int on;
        if (!host_hello || ag_perf_parse(f->data, f->len, &on)) break;
        if (on) perf_apply(); else perf_restore("the host turned it off", 1);
        break;
    }
    case LAG_APPS: {
        int on;
        if (lag_apps_parse(f->data, f->len, &on)) break;
        if (on) apps_start(); else apps_stop();
        break;
    }
    case LAG_WINACT: { LagAct a; if (apps_on && !lag_winact_parse(f->data, f->len, &a)) win_act(&a); break; }
    case LAG_LAUNCH: { const uint8_t *t; uint16_t n; if (apps_on && !lag_launch_parse(f->data, f->len, &t, &n)) launch(t, n); break; }
    case AGK_OPEN: {
        char tok[AG_OPEN_TOKEN + 1];
        uint64_t size;
        uint8_t sha[32];
        if (host_hello && !ag_open_parse(f->data, f->len, tok, &size, sha)) open_begin(tok, size, sha, NULL);
        break;
    }
    case AGK_PLACE: {
        AgPlace pl;
        if (host_hello && apps_on && !ag_place_parse(f->data, f->len, &pl)) open_begin(pl.token, pl.size, pl.sha, &pl);
        break;
    }
    default: break;                 /* kinds this side doesn't know: skipped */
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_FRAME: {
        Frame *f = (Frame *)lp;
        if ((LONG)wp == reader_gen && port != INVALID_HANDLE_VALUE) on_frame(f);
        free(f);
        return 0;
    }
    case WM_APP_OPENED: open_done((OpenJob *)lp); return 0;
    case WM_APP_SHDATA: {
        ShChunk *c = (ShChunk *)lp;
        if (c->gen == sh.gen && sh.id) {
            uint8_t *b = malloc(4 + c->n);
            if (b) {
                uint8_t h8[8];
                ag_shell_pair_write(h8, sh.id, 0);
                memcpy(b, h8, 4);                       /* the id, then the data */
                memcpy(b + 4, c->d, c->n);
                send_frame(AGK_SHELL_DATA, b, 4 + c->n);
                free(b);
            }
        }
        free(c);
        return 0;
    }
    case WM_APP_SHEXIT:
        if ((LONG)wp == sh.gen && sh.proc) {
            GetExitCodeProcess(sh.proc, &sh.code);
            sh.exited = 1;
            ClosePCFn cl = pc_close();
            if (sh.pc && cl) cl(sh.pc);                 /* the rest of its output, then the reader's pipe breaks */
            sh.pc = NULL;
        }
        return 0;
    case WM_APP_SHEOF:
        if ((LONG)wp == sh.gen && sh.id) {
            uint32_t id = sh.id;
            int st = sh.exited ? (sh.code > 255 ? 255 : (int)sh.code) : AG_SHELL_NOSTART;
            shell_end();
            sh_close_frame(id, st);
            lg("shell: ended (%d)", st);
        }
        return 0;
    case WM_COPYDATA: return copydata((const COPYDATASTRUCT *)lp);
    case WM_APP_LOST:
        if ((LONG)wp == reader_gen) { char w[64]; snprintf(w, sizeof w, "reading failed (error %lu)", (unsigned long)lp); port_close(w); }
        return 0;
    case WM_CLIPBOARDUPDATE: SetTimer(h, T_CLIP, 250, NULL); return 0;
    case WM_RENDERFORMAT: clip_render((UINT)wp); return 0;
    case WM_RENDERALLFORMATS:
        /* this program goes away while its offer is on the clipboard: the offer goes too; nothing is fetched for a
         * paste nobody made */
        if (OpenClipboard(h)) { if (GetClipboardOwner() == h) EmptyClipboard(); CloseClipboard(); }
        offer_drop();
        return 0;
    case WM_DESTROYCLIPBOARD: offer_drop(); return 0;
    case WM_TIMER:
        if (wp == T_PORT) {
            if (port != INVALID_HANDLE_VALUE && !console_session()) port_close("another session is at the console");
            port_try_open();
            batch_tick();
            mons_stale = 1;         /* looked at again every 2 s too: a change logged and sent only when there is one */
            mon_get();
        } else if (wp == T_CLIP) { KillTimer(h, T_CLIP); clip_send(); }
        else if (wp == T_DISP) disp_tick();
        else if (wp == T_MONS) {
            KillTimer(h, T_MONS);
            mons_stale = 1;
            mon_get();
            if (apps_on) apps_dirty = 1;
            /* settled (no change of the primary just asked for): windows on another display come onto the shown one */
            else if (rescue_pending && !mons_stale) { rescue_pending = 0; rescue_all("displays", 0); }
            disp_keep();
        }
        else if (wp == T_APPS && apps_on && apps_dirty) { int f = apps_force; apps_dirty = apps_force = 0; send_windows(f); }
        else if (wp == T_TABLET) { KillTimer(h, T_TABLET); tablet_check(); }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_LOCK) { if (!console_session()) port_close("the session left the console"); }
        else port_try_open();
        return 0;
    case WM_DISPLAYCHANGE:
        /* a display came, went or changed its mode: apps mode's next list looks it up again at once (a list made with
         * the corner of before cut the picture in the wrong place: half of one display, half of another), and
         * everything else once things settle */
        mons_stale = 1;
        if (apps_on) apps_dirty = apps_force = 1;
        SetTimer(h, T_MONS, 500, NULL);
        break;
    case WM_INPUT:
        tablet_input((HRAWINPUT)lp);
        break;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) SetTimer(h, T_MONS, 500, NULL);
        break;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wp) { port_close("the person logs off"); } return 0;
    case WM_DESTROY: port_close("exit"); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--port") && i + 1 < argc) { wcsncpy(port_path, argv[++i], 259); port_path[259] = 0; }
        else if (!wcscmp(argv[i], L"--log") && i + 1 < argc) logf = _wfopen(argv[++i], L"a");
        else if (!wcscmp(argv[i], L"--version")) { MessageBoxW(NULL, L"lordos-guest-win " VERSION, L"LordOS", MB_OK); return 0; }
        else if ((!wcscmp(argv[i], L"--open-on-host") && i + 2 == argc) || (!wcscmp(argv[i], L"--notify-host") && i + 1 < argc && i + 3 >= argc))
            return send_to_tools(argv[i], argv[i + 1], i + 2 < argc ? argv[i + 2] : NULL);
    }
    /* the log: %LOCALAPPDATA%\LordOS\lordos-guest-win.log, the person's own, started again past 256 KB */
    if (!logf) {
        wchar_t *la = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la))) {
            wchar_t p[MAX_PATH];
            _snwprintf(p, MAX_PATH, L"%ls\\LordOS", la);
            p[MAX_PATH - 1] = 0;
            CreateDirectoryW(p, NULL);
            _snwprintf(p, MAX_PATH, L"%ls\\LordOS\\lordos-guest-win.log", la);
            p[MAX_PATH - 1] = 0;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            int big = GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (256u << 10));
            logf = _wfopen(p, big ? L"w" : L"a");
            CoTaskMemFree(la);
        }
    }
    /* one per session */
    HANDLE mx = CreateMutexW(NULL, TRUE, L"Local\\LordOS-guest-win");
    if (!mx || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    /* the picture's own pixels, whatever the scale */
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *DpiFn)(HANDLE);
    DpiFn dpi = u ? (DpiFn)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!dpi || !dpi((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ProcessIdToSessionId(GetCurrentProcessId(), &my_session);
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    wr_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    ans_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&ans_cs);
    cf_html = RegisterClipboardFormatW(L"HTML Format");
    cf_png = RegisterClipboardFormatW(L"PNG");
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LordOSGuestWin";
    RegisterClassW(&wc);
    win = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"LordOS guest tools", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!win) return 1;
    AddClipboardFormatListener(win);
    /* the mice's raw input, read in the background for how the tablet maps (above) */
    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, win };
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) lg("tablet: raw input refused (%lu): the host assumes the primary display", (unsigned long)GetLastError());
    WTSRegisterSessionNotification(win, NOTIFY_FOR_THIS_SESSION);
    lg("lordos-guest-win %s in session %lu", VERSION, my_session);
    perf_recover();                     /* a performance profile an earlier run didn't put back */
    SetTimer(win, T_PORT, 2000, NULL);
    SetTimer(win, T_MONS, 3000, NULL);  /* the displays once the session settles: the primary, windows brought back */
    port_try_open();
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
