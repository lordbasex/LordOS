/* rdeskrdp.c — RDP through libfreerdp, with its client library only for shared folders: no display. Its software GDI draws into the
 * shared picture. The connected socket is handed over as FreeRDP's "|fd" host, so the library makes no socket of its own. */
#include "proto.h"

#include <freerdp/addin.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <winpr/synch.h>

#define CH 3
void rd_send(uint32_t type, int a, int b, int c, const char *text);
uint32_t *rd_picture(int w, int h);
PVIRTUALCHANNELENTRY rd_addin_provider(LPCSTR name, LPCSTR subsystem, LPCSTR type, DWORD flags);    /* rdesksnd.c */

static BYTE *picture(int w, int h) { return (BYTE *)rd_picture(w, h); }

static BOOL begin_paint(rdpContext *c) { (void)c; return TRUE; }
static BOOL end_paint(rdpContext *c) {
    rdpGdi *g = c->gdi;
    if (g->primary->hdc->hwnd->invalid->null) return TRUE;
    rd_send(RD_FRAME, 0, 0, 0, NULL);
    return TRUE;
}

static BOOL pre_connect(freerdp *in) {
    (void)in;
    return TRUE;
}

static BOOL post_connect(freerdp *in) {
    rdpSettings *s = in->context->settings;
    int w = (int)freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth), h = (int)freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);
    BYTE *px = picture(w, h);
    if (!px || !gdi_init_ex(in, PIXEL_FORMAT_BGRA32, (UINT32)w * 4, px, NULL)) return FALSE;
    in->context->update->BeginPaint = begin_paint;
    in->context->update->EndPaint = end_paint;
    return TRUE;
}

/* the person decides on an unknown certificate: the app compares it with the one it keeps (sdk/remotecert.js), shows the
 * fingerprint and answers. The library's own store isn't used: a sandbox can't write it, so "changed" comes from the app. */
static DWORD verify(freerdp *in, const char *host, UINT16 port, const char *cn, const char *sub, const char *iss, const char *fp, DWORD flags) {
    (void)in; (void)host; (void)port; (void)cn; (void)sub; (void)iss; (void)flags;
    rd_send(RD_CERT, 0, 0, 0, fp);
    for (;;) {
        RdMsg m;
        if (recv(CH, &m, sizeof m, 0) != (ssize_t)sizeof m || m.type == RD_CLOSE) return 0;
        if (m.type == RD_CERT_OK) return m.a ? 2 : 0;       /* 2: for this session; the app keeps it, not the library */
    }
}
static DWORD verify_changed(freerdp *in, const char *host, UINT16 port, const char *cn, const char *sub, const char *iss, const char *fp,
                            const char *osub, const char *oiss, const char *ofp, DWORD flags) {
    (void)osub; (void)oiss; (void)ofp;
    return verify(in, host, port, cn, sub, iss, fp, flags);
}

static void input(freerdp *in, const RdMsg *m) {
    rdpInput *i = in->context->input;
    if (m->type == RD_KEY) freerdp_input_send_keyboard_event_ex(i, m->c, FALSE, (UINT32)m->b);
    else if (m->type == RD_PTR) {
        static int last;
        int b = m->c;
        static const struct { int bit; UINT16 flag; } map[3] = { { 1, PTR_FLAGS_BUTTON1 }, { 2, PTR_FLAGS_BUTTON3 }, { 4, PTR_FLAGS_BUTTON2 } };
        freerdp_input_send_mouse_event(i, PTR_FLAGS_MOVE, (UINT16)m->a, (UINT16)m->b);
        for (int k = 0; k < 3; k++) if ((b ^ last) & map[k].bit) freerdp_input_send_mouse_event(i, (UINT16)(map[k].flag | ((b & map[k].bit) ? PTR_FLAGS_DOWN : 0)), (UINT16)m->a, (UINT16)m->b);
        if (b & 8) freerdp_input_send_mouse_event(i, PTR_FLAGS_WHEEL | 0x78, (UINT16)m->a, (UINT16)m->b);
        if (b & 16) freerdp_input_send_mouse_event(i, PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x88, (UINT16)m->a, (UINT16)m->b);
        last = b & 7;
    }
}

int rdesk_rdp(int sock, const char *host, const char *user, const char *domain, const char *password, const RdView *view) {
    (void)host;
    freerdp *in = freerdp_new();
    if (!in) return 1;
    in->ContextSize = sizeof(rdpContext);
    if (!freerdp_context_new(in)) return 1;
    in->PreConnect = pre_connect;
    in->PostConnect = post_connect;
    in->VerifyCertificateEx = verify;
    in->VerifyChangedCertificateEx = verify_changed;
    rdpSettings *s = in->context->settings;
    char fdhost[32];
    snprintf(fdhost, sizeof fdhost, "|%d", sock);
    freerdp_settings_set_string(s, FreeRDP_ServerHostname, fdhost);
    freerdp_settings_set_uint32(s, FreeRDP_ServerPort, (UINT32)sock);    /* with a "|" host, the port is the descriptor */
    if (user[0]) freerdp_settings_set_string(s, FreeRDP_Username, user);
    if (domain[0]) freerdp_settings_set_string(s, FreeRDP_Domain, domain);
    if (password[0]) freerdp_settings_set_string(s, FreeRDP_Password, password);
    freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, view->w ? (UINT32)view->w : 1280);
    freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, view->h ? (UINT32)view->h : 800);
    if (view->admin) freerdp_settings_set_bool(s, FreeRDP_ConsoleSession, TRUE);
    /* the remote desktop's own scale (its text and controls bigger), as the server's display settings would set it;
     * the device factor takes the three values the protocol knows */
    if (view->scale >= 100 && view->scale <= 500) {
        freerdp_settings_set_uint32(s, FreeRDP_DesktopScaleFactor, (UINT32)view->scale);
        freerdp_settings_set_uint32(s, FreeRDP_DeviceScaleFactor, view->scale >= 170 ? 180 : view->scale >= 130 ? 140 : 100);
    }
    if (view->drives || view->sound == RD_SOUND_HERE) {
        freerdp_register_addin_provider(rd_addin_provider, 0);
        in->LoadChannels = freerdp_client_load_channels;
    }
    if (view->drives) {
        freerdp_settings_set_bool(s, FreeRDP_DeviceRedirection, TRUE);
        for (int i = 0; i < view->drives; i++) {
            const char *params[] = { "drive", view->drive[i][0], view->drive[i][1] };
            if (!freerdp_client_add_device_channel(s, 3, params)) return 1;
        }
    }
    /* the sound: played here (our rdpsnd device, into lord-server's ring), left on the remote computer, or none */
    if (view->sound == RD_SOUND_HERE) {
        const char *params[] = { "rdpsnd", "sys:lordos" };
        freerdp_settings_set_bool(s, FreeRDP_AudioPlayback, TRUE);
        if (!freerdp_client_add_static_channel(s, 2, params)) return 1;
    } else if (view->sound == RD_SOUND_THERE) freerdp_settings_set_bool(s, FreeRDP_RemoteConsoleAudio, TRUE);
    freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, view->depth == 16 || view->depth == 24 ? (UINT32)view->depth : 32);
    freerdp_settings_set_bool(s, FreeRDP_SoftwareGdi, TRUE);
    /* TLS or NLA only: RDP's own security has no certificate to check, and the password would go to whoever answered */
    freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, FALSE);
    freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_ExtSecurity, FALSE);
    freerdp_settings_set_bool(s, FreeRDP_NegotiateSecurityLayer, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_IgnoreCertificate, FALSE);
    freerdp_settings_set_bool(s, FreeRDP_AutoAcceptCertificate, FALSE);
    if (!freerdp_connect(in)) {
        UINT32 e = freerdp_get_last_error(in->context);
        rd_send(RD_STATE, e == FREERDP_ERROR_CONNECT_LOGON_FAILURE || e == FREERDP_ERROR_AUTHENTICATION_FAILED || e == FREERDP_ERROR_CONNECT_WRONG_PASSWORD ? RS_DENIED : RS_FAILED, 0, 0, NULL);
        return 1;
    }
    rd_send(RD_STATE, RS_CONNECTED, 0, 0, NULL);
    int lost = 0;
    for (;;) {
        HANDLE ev[64];
        DWORD n = freerdp_get_event_handles(in->context, ev, 64);
        if (!n) break;
        DWORD w = WaitForMultipleObjects(n, ev, FALSE, 20);
        if (w == WAIT_FAILED || !freerdp_check_event_handles(in->context) || freerdp_shall_disconnect_context(in->context)) {
            /* the server says why when it ends the session (sign-out, a disconnect, another connection): no reason is a
             * lost connection, which the app may reconnect */
            lost = !freerdp_error_info(in);
            break;
        }
        struct pollfd p = { CH, POLLIN, 0 };
        while (poll(&p, 1, 0) > 0) {
            RdMsg m;
            if (recv(CH, &m, sizeof m, 0) != (ssize_t)sizeof m || m.type == RD_CLOSE) goto out;
            input(in, &m);
        }
    }
    rd_send(RD_STATE, lost ? RS_DROPPED : RS_ENDED, 0, 0, NULL);
out:
    freerdp_disconnect(in);
    gdi_free(in);
    freerdp_context_free(in);
    freerdp_free(in);
    return 0;
}
