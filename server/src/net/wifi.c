/* wifi.c — Wi-Fi with wpa_supplicant (Debian's, fixed path), root side.
 *
 * For each Wi-Fi interface LordOS manages it runs a wpa_supplicant ("wpa:<if>", a resident
 * process of the helper) with a minimal configuration we write ourselves (only the control
 * socket) and a DHCP client ("if:<if>", dhcp.c) that also translates its notices. Networks
 * are loaded over the control socket: the SSID goes in hexadecimal and the WPA2 key as an
 * already computed PSK (PBKDF2), so no user text reaches wpa_supplicant raw except the
 * WPA3 password (sae_password), which is validated (no line breaks or control characters).
 *
 * Saved networks: /etc/lordos/net/wifi/<ssid in hex>.json (public: ssid and security) and
 * <ssid in hex>.key (0600: the password, "pass=…"). A network saved by the LordOS Imager's first-run
 * file (lordos-firstrun, docs/IMAGER.md) has the already derived key instead ("psk=<64 hex>"), so
 * the password never reaches the card: it joins WPA2/WPA3-transition networks (not WPA3-only ones,
 * which need the password itself for SAE). */
#include "netpriv.h"
#include "../admin.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WPA_BIN "/usr/sbin/wpa_supplicant"
#define WPA_DIR NET_RUN_DIR "/wpa"

static int ctrl_open(const char *ifname, char *mypath, size_t n) {
    static unsigned counter;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un me = { .sun_family = AF_UNIX }, srv = { .sun_family = AF_UNIX };
    snprintf(me.sun_path, sizeof me.sun_path, WPA_DIR "/c-%d-%u", (int)getpid(), counter++);
    unlink(me.sun_path);
    snprintf(srv.sun_path, sizeof srv.sun_path, WPA_DIR "/%s", ifname);
    if (bind(fd, (struct sockaddr *)&me, sizeof me) || connect(fd, (struct sockaddr *)&srv, sizeof srv)) {
        unlink(me.sun_path);
        close(fd);
        return -1;
    }
    snprintf(mypath, n, "%s", me.sun_path);
    return fd;
}

/* Receives the reply to a command, skipping notices ("<3>CTRL-EVENT-..."). */
static int ctrl_recv(int fd, char *reply, size_t n, int timeout_ms) {
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, timeout_ms) <= 0) return -1;
        ssize_t r = recv(fd, reply, n - 1, 0);
        if (r < 0) return -1;
        reply[r] = 0;
        if (reply[0] != '<') return (int)r;
    }
}

int wifi_ctrl(const char *ifname, const char *cmd, char *reply, size_t n) {
    char me[108];
    int fd = ctrl_open(ifname, me, sizeof me);
    if (fd < 0) return -1;
    int r = -1;
    if (send(fd, cmd, strlen(cmd), 0) == (ssize_t)strlen(cmd)) r = ctrl_recv(fd, reply, n, 5000);
    close(fd);
    unlink(me);
    return r;
}

static int ctrl_ok(const char *ifname, const char *cmd) {
    char r[64];
    return wifi_ctrl(ifname, cmd, r, sizeof r) > 0 && !strncmp(r, "OK", 2) ? 0 : -1;
}

static void hex(const uint8_t *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]);
    out[2 * n] = 0;
}

/* SSID: 1–32 bytes, no 0. */
static int valid_ssid(const char *s) { size_t n = strlen(s); return n >= 1 && n <= 32; }

static int valid_pass(const char *p) {
    size_t n = strlen(p);
    if (n > 128) return 0;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++) if (*c < 0x20 || *c == 0x7f) return 0;
    return 1;
}

/* ID of the network in wpa_supplicant whose id_str = hex of the SSID, or -1. */
static int find_network(const char *ifname, const char *ssidhex) {
    char list[4096];
    if (wifi_ctrl(ifname, "LIST_NETWORKS", list, sizeof list) <= 0) return -1;
    char *l = strchr(list, '\n');                 /* the first line is the header */
    while (l && *++l) {
        int id = atoi(l);
        char cmd[64], v[128];
        snprintf(cmd, sizeof cmd, "GET_NETWORK %d id_str", id);
        if (wifi_ctrl(ifname, cmd, v, sizeof v) > 0) {
            v[strcspn(v, "\n")] = 0;
            char want[80];
            snprintf(want, sizeof want, "\"%s\"", ssidhex);
            if (!strcmp(v, want)) return id;
        }
        l = strchr(l, '\n');
    }
    return -1;
}

static int is_psk_hex(const char *p) {
    if (strlen(p) != 64) return 0;
    for (const char *c = p; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return 0;
    return 1;
}

/* Loads (or replaces) a network in wpa_supplicant. Empty pass = open. psk (or NULL): a derived
 * WPA2 key in hex, used instead of the password (WPA2 only). Returns the id. */
static int load_network_psk(const char *ifname, const char *ssid, const char *pass, const char *psk) {
    char sh[72], cmd[400], r[64];
    hex((const uint8_t *)ssid, strlen(ssid), sh);
    int old = find_network(ifname, sh);
    if (old >= 0) { snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", old); ctrl_ok(ifname, cmd); }
    if (wifi_ctrl(ifname, "ADD_NETWORK", r, sizeof r) <= 0 || !strncmp(r, "FAIL", 4)) return -1;
    int id = atoi(r);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, sh);
    int ok = ctrl_ok(ifname, cmd) == 0;
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d id_str \"%s\"", id, sh);
    ok = ok && ctrl_ok(ifname, cmd) == 0;
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d scan_ssid 1", id);
    ok = ok && ctrl_ok(ifname, cmd) == 0;
    if (psk) {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt WPA-PSK WPA-PSK-SHA256", id);
        ok = ok && ctrl_ok(ifname, cmd) == 0;
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk %s", id, psk);
        ok = ok && ctrl_ok(ifname, cmd) == 0;
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d ieee80211w 1", id);
        ctrl_ok(ifname, cmd);
    } else if (!pass[0]) {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id);
        ok = ok && ctrl_ok(ifname, cmd) == 0;
    } else {
        size_t pl = strlen(pass);
        int wpa2 = pl >= 8 && pl <= 63;
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt %s", id, wpa2 ? "WPA-PSK WPA-PSK-SHA256 SAE" : "SAE");
        ok = ok && ctrl_ok(ifname, cmd) == 0;
        if (wpa2) {
            uint8_t psk[32];
            char ph[65];
            pbkdf2_sha1((const uint8_t *)pass, pl, (const uint8_t *)ssid, strlen(ssid), 4096, psk, 32);
            hex(psk, 32, ph);
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk %s", id, ph);
            ok = ok && ctrl_ok(ifname, cmd) == 0;
        }
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d sae_password \"%s\"", id, pass);
        if (ctrl_ok(ifname, cmd)) admin_event("net wifi %s no-sae", ifname);   /* wpa_supplicant without SAE: stays WPA2 */
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d ieee80211w 1", id);
        ctrl_ok(ifname, cmd);
    }
    snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d", id);
    ok = ok && ctrl_ok(ifname, cmd) == 0;
    return ok ? id : -1;
}

static int load_network(const char *ifname, const char *ssid, const char *pass) { return load_network_psk(ifname, ssid, pass, NULL); }

/* What a saved <ssid in hex>.key holds: WIFI_SAVED_PASS (a password), WIFI_SAVED_PSK (a derived
 * key) or WIFI_SAVED_OPEN (nothing usable: an open network, or a file that isn't valid). */
int wifi_saved_kind(const char *keytext) {
    if (!keytext) return WIFI_SAVED_OPEN;
    char pass[160], psk[80] = "";
    pass[0] = 0;
    kv_get(keytext, "pass", 0, pass, sizeof pass);
    kv_get(keytext, "psk", 0, psk, sizeof psk);
    int k = psk[0] ? (is_psk_hex(psk) ? WIFI_SAVED_PSK : WIFI_SAVED_OPEN) : pass[0] && valid_pass(pass) ? WIFI_SAVED_PASS : WIFI_SAVED_OPEN;
    memset(pass, 0, sizeof pass);
    memset(psk, 0, sizeof psk);
    return k;
}

/* Loads one saved network from its .key file (path) into wpa_supplicant. Returns the id or -1. */
static int load_saved(const char *ifname, const char *ssid, const char *path) {
    char *t = read_file(path, 1024, NULL);
    if (!t) return -1;
    char pass[160], psk[80] = "";
    pass[0] = 0;
    kv_get(t, "pass", 0, pass, sizeof pass);
    kv_get(t, "psk", 0, psk, sizeof psk);
    int id = -1;
    if (psk[0]) { if (is_psk_hex(psk)) id = load_network_psk(ifname, ssid, "", psk); }
    else if (valid_pass(pass)) id = load_network(ifname, ssid, pass);
    memset(pass, 0, sizeof pass);
    memset(psk, 0, sizeof psk);
    memset(t, 0, strlen(t));
    free(t);
    return id;
}

static void load_known(const char *ifname) {
    DIR *d = opendir(NET_WIFI_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".key") || l - 4 > 64) continue;
        char sh[72], ssid[40], path[300];
        memcpy(sh, e->d_name, l - 4);
        sh[l - 4] = 0;
        size_t n = 0;
        for (size_t i = 0; i + 1 < l - 4 && n < 32; i += 2) { unsigned v; if (sscanf(sh + i, "%2x", &v) != 1) break; ssid[n++] = (char)v; }
        ssid[n] = 0;
        if (!valid_ssid(ssid)) continue;
        snprintf(path, sizeof path, NET_WIFI_DIR "/%s", e->d_name);
        load_saved(ifname, ssid, path);
    }
    if (d) closedir(d);
}

static void dhcp_entry(void *arg) { dhcp_run((const char *)arg, 1); }

int wifi_start(const char *ifname) {
    static char name_arg[32];
    char wname[40], conf[128];
    snprintf(wname, sizeof wname, "wpa:%s", ifname);
    mkdir(NET_RUN_DIR, 0755);
    mkdir(WPA_DIR, 0700);
    if (!admin_daemon_running(wname)) {
        if (access(WPA_BIN, X_OK)) return -1;
        /* one left over from a previous session (the helper died without being able to stop it) */
        char pidf[128];
        snprintf(pidf, sizeof pidf, WPA_DIR "/%s.pid", ifname);
        char *pt = read_file(pidf, 32, NULL);
        if (pt) {
            int pid = atoi(pt);
            char comm[64] = "", cp[64];
            snprintf(cp, sizeof cp, "/proc/%d/comm", pid);
            char *cm = pid > 1 ? read_file(cp, sizeof comm - 1, NULL) : NULL;
            if (cm && !strncmp(cm, "wpa_supplicant", 14)) {
                kill(pid, SIGTERM);
                for (int i = 0; i < 30 && kill(pid, 0) == 0; i++) usleep(100000);
            }
            free(cm);
            free(pt);
        }
        snprintf(conf, sizeof conf, WPA_DIR "/%s.conf", ifname);
        const char *c = "ctrl_interface=" WPA_DIR "\nupdate_config=0\nap_scan=1\n";
        if (write_file_atomic(conf, c, strlen(c), 0600)) return -1;
        const char *argv[] = { WPA_BIN, "-i", ifname, "-c", conf, "-D", "nl80211", "-P", pidf, NULL };
        if (admin_daemon_exec(wname, argv, NULL)) return -1;
        char sock[128];
        snprintf(sock, sizeof sock, WPA_DIR "/%s", ifname);
        for (int i = 0; i < 50 && access(sock, F_OK); i++) usleep(100000);
        if (access(sock, F_OK)) return -1;
        load_known(ifname);
    }
    char dname[40];
    snprintf(dname, sizeof dname, "if:%s", ifname);
    if (!admin_daemon_running(dname)) {
        snprintf(name_arg, sizeof name_arg, "%s", ifname);
        admin_daemon_fork(dname, dhcp_entry, name_arg);
    }
    return 0;
}

/* ---- actions ---- */

static int security_of(const char *flags, char *out, size_t n) {
    if (strstr(flags, "EAP")) snprintf(out, n, "enterprise");
    else if (strstr(flags, "SAE") && !strstr(flags, "PSK")) snprintf(out, n, "wpa3");
    else if (strstr(flags, "SAE")) snprintf(out, n, "wpa2/wpa3");
    else if (strstr(flags, "WPA2") || strstr(flags, "RSN")) snprintf(out, n, "wpa2");
    else if (strstr(flags, "WPA")) snprintf(out, n, "wpa");
    else if (strstr(flags, "WEP")) snprintf(out, n, "wep");
    else snprintf(out, n, "open");
    return 0;
}

/* wpa_supplicant writes the SSID with printf escapes (\\, \", \xNN). */
static void unescape_ssid(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '\\' && p[1] == 'x' && p[2] && p[3]) { unsigned v; if (sscanf(p + 2, "%2x", &v) == 1) { out[o++] = (char)v; p += 3; continue; } }
        if (*p == '\\' && p[1]) { out[o++] = p[1]; p++; continue; }
        out[o++] = *p;
    }
    out[o] = 0;
}

void wifi_act_scan(const char *ifname) {
    char me[108];
    int fd = ctrl_open(ifname, me, sizeof me);
    if (fd < 0) { admin_err("wifi.noSupplicant", "name", ifname, NULL); return; }
    char r[256];
    int got = 0;
    if (send(fd, "ATTACH", 6, 0) == 6 && ctrl_recv(fd, r, sizeof r, 3000) > 0 && send(fd, "SCAN", 4, 0) == 4) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            int left = 10000 - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
            struct pollfd pf = { fd, POLLIN, 0 };
            if (left <= 0 || poll(&pf, 1, left) <= 0) break;
            ssize_t k = recv(fd, r, sizeof r - 1, 0);
            if (k <= 0) break;
            r[k] = 0;
            if (strstr(r, "CTRL-EVENT-SCAN-RESULTS") || strstr(r, "CTRL-EVENT-SCAN-FAILED")) { got = 1; break; }
        }
    }
    close(fd);
    unlink(me);
    (void)got;                                   /* if the notice didn't arrive, whatever results there are get used */
    char *res = malloc(65536);
    if (!res) { admin_err("sys.outOfMemory", NULL); return; }
    if (wifi_ctrl(ifname, "SCAN_RESULTS", res, 65536) <= 0) { free(res); admin_err("wifi.noAnswer", NULL); return; }
    /* bssid / frequency / signal / flags / ssid; one entry per SSID (the one with the best signal) */
    struct { char ssid[40], bssid[20], sec[16]; int freq, signal; } nets[96];
    int n = 0;
    char *l = strchr(res, '\n');
    while (l && *++l) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char bssid[20], flags[256], essid[160] = "";
        int freq, sig;
        char *tab4 = NULL;
        int k = 0;
        for (char *p = l; *p; p++) if (*p == '\t' && ++k == 4) { tab4 = p + 1; break; }
        if (sscanf(l, "%19s %d %d %255s", bssid, &freq, &sig, flags) == 4 && tab4) {
            unescape_ssid(tab4, essid, sizeof essid);
            if (essid[0] && strlen(essid) <= 32) {
                int i = 0;
                while (i < n && strcmp(nets[i].ssid, essid)) i++;
                if (i == n && n < 96) { n++; nets[i].signal = -1000; }
                if (i < n && sig > nets[i].signal) {
                    snprintf(nets[i].ssid, sizeof nets[i].ssid, "%s", essid);
                    snprintf(nets[i].bssid, sizeof nets[i].bssid, "%s", bssid);
                    security_of(flags, nets[i].sec, sizeof nets[i].sec);
                    nets[i].freq = freq;
                    nets[i].signal = sig;
                }
            }
        }
        l = e;
    }
    free(res);
    for (int i = 0; i < n; i++) {
        char sh[72], kp[160];
        hex((const uint8_t *)nets[i].ssid, strlen(nets[i].ssid), sh);
        snprintf(kp, sizeof kp, NET_WIFI_DIR "/%s.key", sh);
        Buf b = {0};
        buf_printf(&b, "{\"ssid\":");
        buf_json_str(&b, nets[i].ssid);
        buf_printf(&b, ",\"bssid\":\"%s\",\"freq\":%d,\"signal\":%d,\"security\":\"%s\",\"known\":%s}",
                   nets[i].bssid, nets[i].freq, nets[i].signal, nets[i].sec, access(kp, F_OK) == 0 ? "true" : "false");
        if (b.p) admin_say("LOG", "%s", b.p);
        buf_free(&b);
    }
    admin_say("OK", "%d", n); /* i18n-ok: a count (data) */
}

void wifi_act_connect(const char *ifname, const char *ssid, const char *pass) {
    if (!valid_ssid(ssid)) { admin_err("wifi.invalidSsid", NULL); return; }
    if (!valid_pass(pass) || (pass[0] && strlen(pass) < 8)) { admin_err("wifi.invalidPassword", NULL); return; }
    char sh[72], path[160], keyp[160];
    hex((const uint8_t *)ssid, strlen(ssid), sh);
    snprintf(keyp, sizeof keyp, NET_WIFI_DIR "/%s.key", sh);
    /* No password for a network saved with one (a click on a saved network, GitHub #124): join it
     * with what is saved. Saving it again would turn it into an open network and lose its key. */
    char *saved = pass[0] ? NULL : read_file(keyp, 1024, NULL);
    int keep = saved && wifi_saved_kind(saved) != WIFI_SAVED_OPEN;
    if (saved) { memset(saved, 0, strlen(saved)); free(saved); }
    if (!keep) {
        /* it's saved: public part (ssid, security) and secret part (password) */
        Buf meta = {0};
        buf_printf(&meta, "{\"ssid\":");
        buf_json_str(&meta, ssid);
        buf_printf(&meta, ",\"security\":\"%s\"}\n", pass[0] ? "psk" : "open");
        snprintf(path, sizeof path, NET_WIFI_DIR "/%s.json", sh);
        int ok = meta.p && write_file_atomic(path, meta.p, meta.len, 0644) == 0;
        buf_free(&meta);
        char sec[200];
        int sl = snprintf(sec, sizeof sec, "pass=%s\n", pass);
        ok = ok && write_file_atomic(keyp, sec, (size_t)sl, 0600) == 0;
        memset(sec, 0, sizeof sec);
        if (!ok) { admin_err("wifi.saveFailed", NULL); return; }
    }
    char wname[40];
    snprintf(wname, sizeof wname, "wpa:%s", ifname);
    int fresh = !admin_daemon_running(wname);
    if (wifi_start(ifname)) { admin_err("wifi.startFailed", "name", ifname, NULL); return; }
    int id = fresh ? find_network(ifname, sh) : keep ? load_saved(ifname, ssid, keyp) : load_network(ifname, ssid, pass);
    if (id < 0) { admin_err("wifi.rejected", NULL); return; }
    char cmd[48];
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
    if (ctrl_ok(ifname, cmd)) { admin_err("wifi.selectFailed", NULL); return; }
    ctrl_ok(ifname, "ENABLE_NETWORK all");        /* the other saved ones stay available */
    admin_ok("wifi.connecting", "ssid", ssid, NULL);
}

void wifi_act_disconnect(const char *ifname) {
    if (ctrl_ok(ifname, "DISCONNECT")) { admin_err("wifi.noSupplicant", "name", ifname, NULL); return; }
    admin_ok("wifi.disconnected", NULL);
}

void wifi_act_forget(const char *ssid) {
    if (!valid_ssid(ssid)) { admin_err("wifi.invalidSsid", NULL); return; }
    char sh[72], path[160];
    hex((const uint8_t *)ssid, strlen(ssid), sh);
    snprintf(path, sizeof path, NET_WIFI_DIR "/%s.key", sh);
    int had = unlink(path) == 0;
    snprintf(path, sizeof path, NET_WIFI_DIR "/%s.json", sh);
    had = (unlink(path) == 0) || had;
    /* remove it from the wpa_supplicants that are running */
    DIR *d = opendir(WPA_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, '.') || !strncmp(e->d_name, "c-", 2) || !strncmp(e->d_name, "ev-", 3)) continue;
        if (!valid_ifname(e->d_name)) continue;
        int id = find_network(e->d_name, sh);
        if (id >= 0) { char cmd[48]; snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id); ctrl_ok(e->d_name, cmd); }
    }
    if (d) closedir(d);
    if (had) admin_ok("wifi.forgotten", NULL); else admin_err("wifi.notSaved", NULL);
}
