/* cups.c — see cups.h. */
#include "cups.h"
#include "ipp.h"
#include "phttp.h"

#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *cups_socket = "/run/cups/cups.sock";
static uint32_t reqid = 1;

static const char *user(void) {
    static char name[64];
    if (!name[0]) {
        struct passwd *pw = getpwuid(getuid());
        snprintf(name, sizeof name, "%s", pw ? pw->pw_name : "lord");
    }
    return name;
}

static void err(JBuf *out, const char *code, const char *detail) {
    jb_free(out);
    jb_raw(out, "{\"error\":");
    jb_str(out, code);
    jb_raw(out, ",\"detail\":");
    jb_str(out, detail ? detail : "");
    jb_raw(out, "}");
}

/* Sends an IPP request to CUPS and decodes the reply. On 401, retries with PeerCred. */
static int do_ipp(const char *path, IppBuf *req, int docfd, size_t doclen, IppMsg *res, PResp *raw, int timeout_ms, JBuf *out) {
    if (req->err) { err(out, "encode", ""); return -1; }
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = phttp_unix(cups_socket);
        if (fd < 0) { err(out, "no-cups", "socket"); return -1; }
        char auth[128] = "";
        if (attempt) snprintf(auth, sizeof auth, "Authorization: PeerCred %s\r\n", user());
        int r = phttp_request(fd, "POST", "localhost", path, auth[0] ? auth : NULL, "application/ipp", req->p, req->n,
                              docfd, doclen, 8 << 20, timeout_ms, raw);
        close(fd);
        if (r) { err(out, "no-cups", "http"); return -1; }
        if (raw->status == 401 && !attempt) { presp_free(raw); continue; }
        if (raw->status == 401 || raw->status == 403) { presp_free(raw); err(out, "forbidden", "lpadmin"); return -1; }
        if (raw->status != 200) { char d[16]; snprintf(d, sizeof d, "%d", raw->status); presp_free(raw); err(out, "http", d); return -1; }
        if (ipp_parse((const uint8_t *)raw->body, raw->len, res)) { presp_free(raw); err(out, "bad-response", ""); return -1; }
        if (res->code >= 0x0400) {
            const IppAttr *m = ipp_find(res, NULL, IPP_G_OPERATION, "status-message");
            const char *code = res->code == 0x0401 || res->code == 0x0402 || res->code == 0x0403 ? "forbidden" :
                               res->code == 0x0406 ? "not-found" : ipp_status_name(res->code);
            err(out, code, ipp_str(m));
            ipp_msg_free(res);
            presp_free(raw);
            return -1;
        }
        return 0;
    }
    return -1;
}

static void put_keywords(JBuf *b, const char *key, const IppAttr *a) {
    jb_printf(b, ",\"%s\":[", key);
    int i = 0;
    for (const IppVal *v = a ? a->vals : NULL; v && i < 200; v = v->next, i++) {
        if (i) jb_raw(b, ",");
        jb_str(b, v->str ? v->str : "");
    }
    jb_raw(b, "]");
}

static const char *const printer_attrs[] = {
    "printer-name", "printer-info", "printer-location", "printer-make-and-model", "printer-state", "printer-state-message",
    "printer-state-reasons", "printer-is-accepting-jobs", "device-uri", "printer-uri-supported", "media-supported",
    "media-default", "sides-supported", "sides-default", "print-color-mode-supported", "print-color-mode-default",
    "color-supported", "printer-type", "queued-job-count",
};

static void add_requested(IppBuf *b, const char *const *names, int n) {
    for (int i = 0; i < n; i++) ipp_add_str(b, IPP_T_KEYWORD, i ? NULL : "requested-attributes", names[i]);
}

int cups_printers(JBuf *out) {
    /* the default one */
    char def[128] = "";
    {
        IppBuf q;
        ipp_init(&q, IPP_OP_CUPS_GET_DEFAULT, reqid++);
        ipp_std(&q, NULL, user());
        ipp_add_str(&q, IPP_T_KEYWORD, "requested-attributes", "printer-name");
        ipp_end(&q);
        IppMsg m;
        PResp raw;
        JBuf tmp;
        jb_init(&tmp);
        if (!do_ipp("/", &q, -1, 0, &m, &raw, 8000, &tmp)) {
            const char *n = ipp_str(ipp_find(&m, NULL, IPP_G_PRINTER, "printer-name"));
            if (n) snprintf(def, sizeof def, "%s", n);
            ipp_msg_free(&m);
            presp_free(&raw);
        } else if (strstr(tmp.p ? tmp.p : "", "no-cups")) {
            ipp_free(&q);
            jb_free(out);
            *out = tmp;
            return -1;
        }
        jb_free(&tmp);
        ipp_free(&q);
    }
    IppBuf q;
    ipp_init(&q, IPP_OP_CUPS_GET_PRINTERS, reqid++);
    ipp_std(&q, NULL, user());
    add_requested(&q, printer_attrs, (int)(sizeof printer_attrs / sizeof printer_attrs[0]));
    ipp_end(&q);
    IppMsg m;
    PResp raw;
    int r = do_ipp("/", &q, -1, 0, &m, &raw, 10000, out);
    ipp_free(&q);
    if (r) {
        /* with no printers CUPS answers not-found: empty list */
        if (out->p && strstr(out->p, "not-found")) { jb_free(out); jb_printf(out, "{\"printers\":[],\"default\":\"\"}"); return 0; }
        return -1;
    }
    jb_free(out);
    jb_raw(out, "{\"default\":");
    jb_str(out, def);
    jb_raw(out, ",\"printers\":[");
    int n = 0;
    for (const IppAttr *a = m.attrs; a; a = a->next) {
        if (a->group != IPP_G_PRINTER || strcmp(a->name, "printer-name")) continue;
        /* this printer's group: from its first attribute */
        const IppAttr *g = m.attrs;
        while (g && g->gseq != a->gseq) g = g->next;
        if (n++) jb_raw(out, ",");
        jb_raw(out, "{\"name\":");
        jb_str(out, ipp_str(a));
        static const char *const texts[][2] = { { "info", "printer-info" }, { "location", "printer-location" }, { "model", "printer-make-and-model" },
                                                { "message", "printer-state-message" }, { "uri", "device-uri" }, { "mediaDefault", "media-default" },
                                                { "sidesDefault", "sides-default" }, { "colorDefault", "print-color-mode-default" } };
        for (size_t k = 0; k < sizeof texts / sizeof texts[0]; k++) {
            jb_printf(out, ",\"%s\":", texts[k][0]);
            jb_str(out, ipp_str(ipp_find_in(g, texts[k][1])));
        }
        jb_printf(out, ",\"state\":%d,\"accepting\":%s,\"color\":%s,\"queued\":%d", ipp_int(ipp_find_in(g, "printer-state"), 3),
                  ipp_int(ipp_find_in(g, "printer-is-accepting-jobs"), 1) ? "true" : "false",
                  ipp_int(ipp_find_in(g, "color-supported"), 0) ? "true" : "false", ipp_int(ipp_find_in(g, "queued-job-count"), 0));
        put_keywords(out, "media", ipp_find_in(g, "media-supported"));
        put_keywords(out, "sides", ipp_find_in(g, "sides-supported"));
        put_keywords(out, "colorModes", ipp_find_in(g, "print-color-mode-supported"));
        put_keywords(out, "reasons", ipp_find_in(g, "printer-state-reasons"));
        jb_raw(out, "}");
    }
    jb_raw(out, "]}");
    ipp_msg_free(&m);
    presp_free(&raw);
    return 0;
}

int cups_jobs(JBuf *out) {
    IppBuf q;
    ipp_init(&q, IPP_OP_GET_JOBS, reqid++);
    ipp_std(&q, "ipp://localhost/", user());
    ipp_add_str(&q, IPP_T_KEYWORD, "which-jobs", "not-completed");
    static const char *const attrs[] = { "job-id", "job-name", "job-state", "job-originating-user-name", "job-printer-uri",
                                         "time-at-creation", "job-k-octets", "job-media-sheets-completed", "job-state-reasons" };
    add_requested(&q, attrs, (int)(sizeof attrs / sizeof attrs[0]));
    ipp_end(&q);
    IppMsg m;
    PResp raw;
    int r = do_ipp("/", &q, -1, 0, &m, &raw, 10000, out);
    ipp_free(&q);
    if (r) {
        if (out->p && strstr(out->p, "not-found")) { jb_free(out); jb_raw(out, "{\"jobs\":[]}"); return 0; }
        return -1;
    }
    jb_free(out);
    jb_raw(out, "{\"jobs\":[");
    int n = 0;
    for (const IppAttr *a = m.attrs; a; a = a->next) {
        if (a->group != IPP_G_JOB || strcmp(a->name, "job-id")) continue;
        const IppAttr *g = m.attrs;
        while (g && g->gseq != a->gseq) g = g->next;
        const char *puri = ipp_str(ipp_find_in(g, "job-printer-uri"));
        const char *pname = puri && strrchr(puri, '/') ? strrchr(puri, '/') + 1 : "";
        if (n++) jb_raw(out, ",");
        jb_printf(out, "{\"id\":%d,\"state\":%d,\"created\":%d,\"kb\":%d,\"sheets\":%d,\"name\":", ipp_int(a, 0),
                  ipp_int(ipp_find_in(g, "job-state"), 3), ipp_int(ipp_find_in(g, "time-at-creation"), 0),
                  ipp_int(ipp_find_in(g, "job-k-octets"), 0), ipp_int(ipp_find_in(g, "job-media-sheets-completed"), 0));
        jb_str(out, ipp_str(ipp_find_in(g, "job-name")));
        jb_raw(out, ",\"user\":");
        jb_str(out, ipp_str(ipp_find_in(g, "job-originating-user-name")));
        jb_raw(out, ",\"printer\":");
        jb_str(out, pname);
        jb_raw(out, ",\"reason\":");
        jb_str(out, ipp_str(ipp_find_in(g, "job-state-reasons")));
        jb_raw(out, "}");
    }
    jb_raw(out, "]}");
    ipp_msg_free(&m);
    presp_free(&raw);
    return 0;
}

int cups_cancel(int job, JBuf *out) {
    if (job <= 0) { err(out, "bad-request", "job"); return -1; }
    char uri[64];
    snprintf(uri, sizeof uri, "ipp://localhost/jobs/%d", job);
    IppBuf q;
    ipp_init(&q, IPP_OP_CANCEL_JOB, reqid++);
    ipp_group(&q, IPP_G_OPERATION);
    ipp_add_str(&q, IPP_T_CHARSET, "attributes-charset", "utf-8");
    ipp_add_str(&q, IPP_T_LANGUAGE, "attributes-natural-language", "en");
    ipp_add_str(&q, IPP_T_URI, "job-uri", uri);
    ipp_add_str(&q, IPP_T_NAME, "requesting-user-name", user());
    ipp_end(&q);
    IppMsg m;
    PResp raw;
    int r = do_ipp("/jobs", &q, -1, 0, &m, &raw, 10000, out);
    ipp_free(&q);
    if (r) return -1;
    ipp_msg_free(&m);
    presp_free(&raw);
    jb_free(out);
    jb_printf(out, "{\"job\":%d}", job);
    return 0;
}

int cups_valid_name(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 127) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') return 0;
    return 1;
}

int cups_valid_uri(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 8 || n > 1000) return 0;
    if (strncmp(s, "ipp://", 6) && strncmp(s, "ipps://", 7) && strncmp(s, "http://", 7) && strncmp(s, "https://", 8) && strncmp(s, "dnssd://", 8)) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] <= 0x20 || s[i] == 0x7f || s[i] == '"' || s[i] == '\\') return 0;
    return 1;
}

int cups_valid_text(const char *s, size_t max) {
    if (!s) return 1;
    size_t n = strlen(s);
    if (n > max) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20) return 0;
    return 1;
}

int cups_page_ranges(const char *s, int lo[], int hi[], int max) {
    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p) || n >= max) return -1;
        long a = strtol(p, (char **)&p, 10), b = a;
        while (*p == ' ') p++;
        if (*p == '-') { p++; while (*p == ' ') p++; if (!isdigit((unsigned char)*p)) return -1; b = strtol(p, (char **)&p, 10); }
        if (a < 1 || b < a || b > 100000) return -1;
        lo[n] = (int)a;
        hi[n] = (int)b;
        n++;
        while (*p == ' ') p++;
        if (*p && *p != ',') return -1;
    }
    return n;
}

int cups_submit(const char *printer, int docfd, size_t len, const JVal *o, JBuf *out) {
    if (!cups_valid_name(printer)) { err(out, "bad-request", "printer"); return -1; }
    char uri[192], path[160];
    snprintf(uri, sizeof uri, "ipp://localhost/printers/%s", printer);
    snprintf(path, sizeof path, "/printers/%s", printer);
    IppBuf q;
    ipp_init(&q, IPP_OP_PRINT_JOB, reqid++);
    ipp_std(&q, uri, user());
    const char *name = json_str(o, "name");
    ipp_add_str(&q, IPP_T_NAME, "job-name", name && cups_valid_text(name, 200) && name[0] ? name : "Axia DE");
    ipp_add_str(&q, IPP_T_MIMETYPE, "document-format", "application/pdf");
    ipp_group(&q, IPP_G_JOB);
    int copies = (int)json_num(o, "copies", 1);
    if (copies < 1 || copies > 999) copies = 1;
    ipp_add_int(&q, IPP_T_INTEGER, "copies", copies);
    const char *pages = json_str(o, "pages");
    if (pages && pages[0]) {
        int lo[32], hi[32];
        int n = cups_page_ranges(pages, lo, hi, 32);
        if (n < 0) { ipp_free(&q); err(out, "bad-pages", pages); return -1; }
        for (int i = 0; i < n; i++) ipp_add_range(&q, i ? NULL : "page-ranges", lo[i], hi[i]);
    }
    const char *sides = json_str(o, "sides");
    if (sides && (!strcmp(sides, "one-sided") || !strcmp(sides, "two-sided-long-edge") || !strcmp(sides, "two-sided-short-edge")))
        ipp_add_str(&q, IPP_T_KEYWORD, "sides", sides);
    const char *color = json_str(o, "color");
    if (color && (!strcmp(color, "color") || !strcmp(color, "monochrome") || !strcmp(color, "auto")))
        ipp_add_str(&q, IPP_T_KEYWORD, "print-color-mode", color);
    const char *media = json_str(o, "media");
    if (media && media[0] && strlen(media) < 64) {
        int ok = 1;
        for (const char *p = media; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-') ok = 0;
        if (ok) ipp_add_str(&q, IPP_T_KEYWORD, "media", media);
    }
    const char *orient = json_str(o, "orientation");
    if (orient && !strcmp(orient, "landscape")) ipp_add_int(&q, IPP_T_ENUM, "orientation-requested", 4);
    ipp_end(&q);
    IppMsg m;
    PResp raw;
    int r = do_ipp(path, &q, docfd, len, &m, &raw, 60000, out);
    ipp_free(&q);
    if (r) return -1;
    int job = ipp_int(ipp_find(&m, NULL, IPP_G_JOB, "job-id"), 0);
    ipp_msg_free(&m);
    presp_free(&raw);
    jb_free(out);
    jb_printf(out, "{\"job\":%d}", job);
    return 0;
}

static int admin_op(uint16_t op, const char *name, JBuf *out, void (*more)(IppBuf *, const void *), const void *ud, int timeout) {
    if (!cups_valid_name(name)) { err(out, "bad-request", "name"); return -1; }
    char uri[192];
    snprintf(uri, sizeof uri, "ipp://localhost/printers/%s", name);
    IppBuf q;
    ipp_init(&q, op, reqid++);
    ipp_std(&q, uri, user());
    if (more) more(&q, ud);
    ipp_end(&q);
    IppMsg m;
    PResp raw;
    int r = do_ipp("/admin/", &q, -1, 0, &m, &raw, timeout, out);
    ipp_free(&q);
    if (r) return -1;
    ipp_msg_free(&m);
    presp_free(&raw);
    jb_free(out);
    jb_raw(out, "{\"name\":");
    jb_str(out, name);
    jb_raw(out, "}");
    return 0;
}

typedef struct { const char *uri, *info, *location; } AddArgs;

static void add_more(IppBuf *q, const void *ud) {
    const AddArgs *a = ud;
    /* "everywhere" model: CUPS builds the PPD by asking the printer (driverless printing, IPP Everywhere) */
    ipp_add_str(q, IPP_T_NAME, "ppd-name", "everywhere");
    ipp_group(q, IPP_G_PRINTER);
    ipp_add_str(q, IPP_T_URI, "device-uri", a->uri);
    if (a->info && a->info[0]) ipp_add_str(q, IPP_T_TEXT, "printer-info", a->info);
    if (a->location && a->location[0]) ipp_add_str(q, IPP_T_TEXT, "printer-location", a->location);
    ipp_add_bool(q, "printer-is-accepting-jobs", 1);
    ipp_add_int(q, IPP_T_ENUM, "printer-state", 3);
}

int cups_add(const char *name, const char *uri, const char *info, const char *location, JBuf *out) {
    if (!cups_valid_uri(uri) || !cups_valid_text(info, 127) || !cups_valid_text(location, 127)) { err(out, "bad-request", "uri"); return -1; }
    AddArgs a = { uri, info, location };
    return admin_op(IPP_OP_CUPS_ADD_MODIFY_PRINTER, name, out, add_more, &a, 120000);
}

int cups_remove(const char *name, JBuf *out) { return admin_op(IPP_OP_CUPS_DELETE_PRINTER, name, out, NULL, NULL, 20000); }
int cups_set_default(const char *name, JBuf *out) { return admin_op(IPP_OP_CUPS_SET_DEFAULT, name, out, NULL, NULL, 20000); }
