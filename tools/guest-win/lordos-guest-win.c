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
 *   - the display's size (AGK_DISPLAY): the size the machine's window wants, set on the primary display. The VirtIO
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
 *     WM_DROPFILES to a window that takes dropped files, or the clipboard (CF_HDROP) for any other, to paste.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../server/src/vm/vmagent.h"

/* w5 (2026-11-21): "pointer": lord-vm may set the display driver's hardware pointer (viogpudo's HWCursor) through
 * qemu-guest-agent once these tools run (docs/VIRTUALIZATION.md 7.1.2); this program itself doesn't touch it */
/* w6 (2026-11-21): "gvt": lord-vm may install Intel's driver for a GVT-g vGPU, block its updates and turn hibernation
 * off through qemu-guest-agent once these tools run (server/src/vm/vmgvtwin.h); this program itself doesn't touch them */
#define VERSION "w6"
#define HELLO "lordos-guest " VERSION " clipboard clipx clipreq display windows open place pointer gvt"

enum { WM_APP_FRAME = WM_APP + 1, WM_APP_LOST, WM_APP_OPENED };
enum { T_PORT = 1, T_CLIP, T_DISP, T_APPS };

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
static int host_hello, host_clipx;

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
static void port_close(const char *why) {
    if (port == INVALID_HANDLE_VALUE) return;
    lg("port closed: %s", why);
    SetEvent(stop_ev);
    if (reader) { WaitForSingleObject(reader, 5000); CloseHandle(reader); reader = NULL; }
    CloseHandle(port);
    port = INVALID_HANDLE_VALUE;
    InterlockedIncrement(&reader_gen);
    host_hello = host_clipx = 0;
    apps_stop();
    SetEvent(ans_ev);                   /* a paste waiting for the host stops waiting */
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
    send_frame(AGK_HELLO, (const uint8_t *)HELLO, sizeof HELLO - 1);
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

static int primary_device(wchar_t name[32]) {
    DISPLAY_DEVICEW d;
    for (DWORD i = 0;; i++) {
        memset(&d, 0, sizeof d);
        d.cb = sizeof d;
        if (!EnumDisplayDevicesW(NULL, i, &d, 0)) return -1;
        if (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { memcpy(name, d.DeviceName, sizeof d.DeviceName); return 0; }
    }
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
    if (primary_device(dev)) return -1;
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
        if (disp_tries >= 2 && !disp_setconfig(dev, disp_w, disp_h)) {
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
    return r == DISP_CHANGE_SUCCESSFUL ? 1 : -1;
}

static void disp_report(void) {
    wchar_t dev[32];
    DEVMODEW cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (primary_device(dev) || !EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &cur, 0)) return;
    uint8_t o[8];
    if (cur.dmPelsWidth < AG_DISPLAY_MIN_W || cur.dmPelsHeight < AG_DISPLAY_MIN_H || cur.dmPelsWidth > AG_DISPLAY_MAX || cur.dmPelsHeight > AG_DISPLAY_MAX) return;
    ag_display_write(o, cur.dmPelsWidth, cur.dmPelsHeight);
    send_frame(AGK_DISPLAY, o, 8);
}

/* the host's size arrives with (or just before) the driver's own change: the mode is looked for every 250 ms for 4 s */
static void disp_tick(void) {
    disp_tries++;
    if (disp_tries == 4) { wchar_t dev[32]; if (!primary_device(dev)) viogpu_ask(dev, disp_w, disp_h); }
    int r = disp_apply(disp_tries >= 16);
    if (r != 0) { KillTimer(win, T_DISP); disp_report(); }
}

/* ---- apps mode ---- */
static int apps_on;
static HWINEVENTHOOK hooks[2];
static int apps_dirty;
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

/* the window's rectangle as seen (without the invisible resize borders), or the restored one for a minimized window */
static int win_rect(HWND h, RECT *r) {
    if (IsIconic(h)) {
        WINDOWPLACEMENT wp;
        wp.length = sizeof wp;
        if (!GetWindowPlacement(h, &wp)) return -1;
        *r = wp.rcNormalPosition;
        return 0;
    }
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof *r)) && !GetWindowRect(h, r)) return -1;
    return 0;
}

typedef struct { Buf *b; int n; HWND fg; RECT desk; uint32_t ids[LAG_WIN_MAX]; } ListCtx;
static BOOL CALLBACK list_one(HWND h, LPARAM lp) {
    ListCtx *c = (ListCtx *)lp;
    if (c->n >= LAG_WIN_MAX) return FALSE;
    if (!listable(h)) return TRUE;
    RECT r, k;
    if (win_rect(h, &r)) return TRUE;
    if (!IntersectRect(&k, &r, &c->desk)) { if (!IsIconic(h)) return TRUE; k = c->desk; k.right = k.left + 1; k.bottom = k.top + 1; }
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
    c.desk.left = GetSystemMetrics(SM_XVIRTUALSCREEN); c.desk.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    c.desk.right = c.desk.left + GetSystemMetrics(SM_CXVIRTUALSCREEN); c.desk.bottom = c.desk.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
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
    switch (a->action) {
    case LAGA_FOCUS: focus(h); break;
    case LAGA_CLOSE: PostMessageW(h, WM_CLOSE, 0, 0); break;
    case LAGA_MINIMIZE: ShowWindowAsync(h, SW_MINIMIZE); break;
    case LAGA_RESTORE: ShowWindowAsync(h, SW_RESTORE); break;
    case LAGA_MOVE: {
        /* the list says where the visible frame is: the invisible borders are added back */
        RECT f, w;
        int dl = 0, dt = 0, dr = 0, db = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &f, sizeof f)) && GetWindowRect(h, &w)) {
            dl = f.left - w.left; dt = f.top - w.top; dr = w.right - f.right; db = w.bottom - f.bottom;
        }
        /* never outside the screen: a window put off it is hidden or dropped from the list (the host clamps too; this
         * side keeps it so with an older host, apps mode on several host displays) */
        int x = a->x, y = a->y, ww = a->w, wh = a->h;
        RECT want = { x, y, x + ww, y + wh };
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            RECT m = mi.rcMonitor;
            if (ww > m.right - m.left) ww = m.right - m.left;
            if (wh > m.bottom - m.top) wh = m.bottom - m.top;
            if (x < m.left) x = m.left;
            if (y < m.top) y = m.top;
            if (x + ww > m.right) x = m.right - ww;
            if (y + wh > m.bottom) y = m.bottom - wh;
            if (x != a->x || y != a->y) lg("apps: a move to %d,%d kept on the screen at %d,%d", a->x, a->y, x, y);
        }
        if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
        SetWindowPos(h, NULL, x - dl, y - dt, ww + dl + dr, wh + dt + db, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
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
        POINT pt = { b->x, b->y };
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

/* ---- frames from the host ---- */
static void on_frame(const Frame *f) {
    switch (f->kind) {
    case AGK_HELLO:
        if (f->len < 200) {
            char t[201];
            memcpy(t, f->data, f->len);
            t[f->len] = 0;
            host_hello = 1;
            host_clipx = strstr(t, " clipx") != NULL;
            if (strstr(t, " ask")) send_frame(AGK_HELLO, (const uint8_t *)HELLO, sizeof HELLO - 1);
            lg("host: %s", ag_utf8_ok(f->data, f->len) ? t : "(hello)");
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
        lg("display: the host wants %ux%u", w, h);
        SetTimer(win, T_DISP, 250, NULL);
        disp_tick();
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
        } else if (wp == T_CLIP) { KillTimer(h, T_CLIP); clip_send(); }
        else if (wp == T_DISP) disp_tick();
        else if (wp == T_APPS && apps_on && apps_dirty) { apps_dirty = 0; send_windows(0); }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_LOCK) { if (!console_session()) port_close("the session left the console"); }
        else port_try_open();
        return 0;
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
    WTSRegisterSessionNotification(win, NOTIFY_FOR_THIS_SESSION);
    lg("lordos-guest-win %s in session %lu", VERSION, my_session);
    SetTimer(win, T_PORT, 2000, NULL);
    port_try_open();
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
