/* lan_test.c — Files → Network without a network: the address policy, the arguments of the share-*
 * actions, and every parser of lord-lan against crafted and damaged answers. Built with ASan and UBSan. */
#include "../src/lan/lanaddr.h"
#include "../src/lan/lanparse.h"
#include "../src/lan/smbauth.h"
#include "../src/lan/rconnect.h"
#include "../src/disk/diskkinds.h"
#include "../src/sec/cgroup.h"
#include "../src/lan/lanservices.h"
#include "../src/print/mdns.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint32_t ip(const char *s) { struct in_addr a; inet_pton(AF_INET, s, &a); return ntohl(a.s_addr); }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v); p16(p + 2, v >> 16); }
static void b32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void t_addr(void) {
    CHECK(lan_private(ip("10.1.2.3")) && lan_private(ip("172.16.0.1")) && lan_private(ip("172.31.255.254")));
    CHECK(lan_private(ip("192.168.1.1")) && lan_private(ip("169.254.9.9")));
    CHECK(!lan_private(ip("8.8.8.8")) && !lan_private(ip("172.32.0.1")) && !lan_private(ip("172.15.0.1")));
    CHECK(!lan_private(ip("127.0.0.1")) && !lan_private(ip("100.64.0.1")) && !lan_private(ip("224.0.0.251")) && !lan_private(ip("192.169.0.1")));
    uint32_t f, l;
    CHECK(lan_sweep_range(ip("192.168.1.50"), 0xffffff00u, &f, &l) == 254 && f == ip("192.168.1.1") && l == ip("192.168.1.254"));
    CHECK(lan_sweep_range(ip("10.0.7.9"), 0xffff0000u, &f, &l) == 254 && f == ip("10.0.7.1"));      /* never more than the /24 */
    CHECK(lan_sweep_range(ip("10.0.7.9"), 0, &f, &l) == 254);
    CHECK(lan_sweep_range(ip("10.0.7.9"), 0xfffffff0u, &f, &l) == 14 && f == ip("10.0.7.1"));
    CHECK(lan_sweep_range(ip("10.0.7.9"), 0xfffffffeu, &f, &l) == 0);
    CHECK(lan_sweep_range(ip("10.0.7.9"), 0xffffffffu, &f, &l) == 0);
    char t[16];
    CHECK(lan_clean_text("  a\x01" "b\x7f  ", t, sizeof t) && !strcmp(t, "ab"));
    CHECK(lan_clean_text("\xff\xfe" "ok", t, sizeof t) && !strcmp(t, "ok"));
    CHECK(!lan_clean_text("\x01\x02", t, sizeof t));
    CHECK(lan_clean_text("áááááááááááááááááááá", t, 8) && strlen(t) == 6);      /* never cuts a character */
}

static void hex(const char *h, uint8_t *o) { for (; *h; h += 2) { unsigned v; sscanf(h, "%2x", &v); *o++ = (uint8_t)v; } }
static int same(const uint8_t *a, const char *h) { uint8_t e[64]; hex(h, e); return !memcmp(a, e, strlen(h) / 2); }

/* RFC 1320, RFC 1321, RFC 2202 and the worked example in the NTLM specification */
static void t_smbauth(void) {
    uint8_t o[32];
    md4("", 0, o); CHECK(same(o, "31d6cfe0d16ae931b73c59d7e0c089c0"));
    md4("abc", 3, o); CHECK(same(o, "a448017aaf21d8525fc10ae87aa6729d"));
    const char *d80 = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    md4(d80, 80, o); CHECK(same(o, "e33b4ddc9c38f2199c3e7b164fcc0536"));
    md5("", 0, o); CHECK(same(o, "d41d8cd98f00b204e9800998ecf8427e"));
    md5("abc", 3, o); CHECK(same(o, "900150983cd24fb0d6963f7d28e17f72"));
    md5(d80, 80, o); CHECK(same(o, "57edf4a22be3c955ac49da2e2107b67a"));
    hmac_md5("Jefe", 4, "what do ya want for nothing?", 28, o); CHECK(same(o, "750c783e6ab0b503eaa86e310a5db738"));
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, o); CHECK(same(o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    uint8_t u[32];
    CHECK(text_utf16("añ", u, sizeof u, 1) == 4 && u[0] == 'A' && u[2] == 0xd1 && u[3] == 0);
    CHECK(text_utf16("", u, sizeof u, 0) == 1);
    CHECK(!text_utf16("\xff", u, sizeof u, 0));
    uint8_t hash[16], client[8], server[8], temp[256], proof[16], base[16];
    CHECK(!ntlmv2_hash("User", "Domain", "Password", hash) && same(hash, "0c868a403bfd7a93a3001ef22ef02e3f"));
    memset(client, 0xaa, 8);
    hex("0123456789abcdef", server);
    uint8_t av[36];
    hex("02000c0044006f006d00610069006e0001000c005300650072007600650072000000" "0000", av);
    size_t tl = ntlmv2_temp(temp, sizeof temp, 0, client, av, 36);
    ntlmv2_proof(hash, server, temp, tl, proof, base);
    CHECK(same(proof, "68cd0ab851e51c96aabc927bebef6a1c"));
    CHECK(same(base, "8de40ccadbc14a82f15cb0ad0de95ca3"));
    /* a challenge cut anywhere, or pointing outside itself, is refused */
    uint8_t c[120] = { 0 }, a[1024], key[16], rnd[24] = { 0 }, neg[40];
    memcpy(c, "NTLMSSP", 8); p32(c + 8, 2); p32(c + 20, 0x40000000); p16(c + 40, 36); p16(c + 42, 36); p32(c + 44, 56); memcpy(c + 56, av, 36);
    CHECK(ntlm_user_negotiate(neg, sizeof neg) == 40);
    size_t al = ntlm_user_auth(a, sizeof a, neg, 40, c, 92, "ana", "", "x", "cifs/10.0.0.9", rnd, key);
    CHECK(al > 88);
    /* the target name and "a MIC follows" are in the part the proof covers; the MIC covers the three messages */
    uint8_t want[26]; size_t wl = text_utf16("cifs/10.0.0.9", want, sizeof want, 0);
    int found = 0;
    for (size_t i = 88; i + wl <= al; i++) found |= !memcmp(a + i, want, wl);
    CHECK(found);
    uint8_t all[1500], mic[16], zero[16] = { 0 };
    memcpy(mic, a + 72, 16); CHECK(memcmp(mic, zero, 16));
    memcpy(all, neg, 40); memcpy(all + 40, c, 92); memcpy(all + 132, a, al); memset(all + 132 + 72, 0, 16);
    uint8_t m2[16]; hmac_md5(key, 16, all, 132 + al, m2); CHECK(!memcmp(m2, mic, 16));
    a[100] ^= 1; memcpy(all + 132, a, al); memset(all + 132 + 72, 0, 16); hmac_md5(key, 16, all, 132 + al, m2); CHECK(memcmp(m2, mic, 16));
    for (size_t l = 0; l < 92; l++) CHECK(!ntlm_user_auth(a, sizeof a, neg, 40, c, l, "ana", "", "x", "cifs/h", rnd, key));
    p32(c + 44, 0xfffffff0u); CHECK(!ntlm_user_auth(a, sizeof a, neg, 40, c, 92, "ana", "", "x", "cifs/h", rnd, key));
    uint8_t msg[80] = { 0 }; smb_sign(msg, 80, key); CHECK(msg[16] & 8);
    CHECK(smb_verify(msg, 80, key));
    msg[70] ^= 1; CHECK(!smb_verify(msg, 80, key)); msg[70] ^= 1;
    msg[50] ^= 1; CHECK(!smb_verify(msg, 80, key)); msg[50] ^= 1;
    msg[16] &= (uint8_t)~8; CHECK(!smb_verify(msg, 80, key));       /* not signed at all */
}

static void t_review(void) {
    /* every key stored is one shown */
    char o[512];
    CHECK(!rconnect_prints("256 SHA256:abc h (ED25519)\n3072 SHA256:def h (RSA)\n", o, sizeof o) && !strcmp(o, "ED25519 SHA256:abc · RSA SHA256:def"));
    CHECK(rconnect_prints("", o, sizeof o) && rconnect_prints("garbage\n", o, sizeof o) && rconnect_prints("256 SHA256:abc h (ED25519)\n", o, 8));
    /* an accepted certificate is checked first, even when the system would trust another */
    CHECK(rconnect_tls_decide("A", "A", 1) == RC_TLS_PIN && rconnect_tls_decide("A", "B", 1) == RC_TLS_CHANGED && rconnect_tls_decide("A", "B", 0) == RC_TLS_CHANGED);
    CHECK(rconnect_tls_decide(NULL, "A", 1) == RC_TLS_SYSTEM && rconnect_tls_decide(NULL, "A", 0) == RC_TLS_ASK);
    /* a certificate can't fake the verdict with its own text */
    char pem[256];
    CHECK(rconnect_tls_parse("subject=Verify return code: 0 (ok)\n-----BEGIN CERTIFICATE-----\nx\n-----END CERTIFICATE-----\nVerify return code: 18 (self-signed)\n", pem, sizeof pem) == 0);
    /* a host name holding NOT isn't taken for the negative */
    CHECK(rconnect_name_ok("Hostname NOTES.local does match certificate\n") && rconnect_name_ok("IP address 10.0.0.1 does match certificate\n"));
    CHECK(!rconnect_name_ok("Hostname h does NOT match certificate\n") && !rconnect_name_ok(""));
    /* an answer is about the fingerprint on screen */
    CHECK(rconnect_answer("SHA256:a", "SHA256:a", 1) == 1 && rconnect_answer("SHA256:a", "SHA256:a", 0) == -1);
    CHECK(!rconnect_answer("SHA256:a", "SHA256:b", 1) && !rconnect_answer("", "", 1));
    /* a .desktop can't take a helper's firewall identity; HFS+ never mounts for writing */
    CHECK(app_scope_reserved("lordos-rclone") && app_scope_reserved("lordos-rdesk") && !app_scope_reserved("rclone-browser") && !app_scope_reserved("firefox"));
    int ro = 0, seen = 0;
    for (size_t i = 0; i < sizeof DISK_KINDS / sizeof *DISK_KINDS; i++) if (!strcmp(DISK_KINDS[i].udev, "hfsplus") || !strcmp(DISK_KINDS[i].udev, "apfs")) { seen++; ro += DISK_KINDS[i].ro; }
    CHECK(seen == 2 && ro == 2);
}

static void t_rconnect(void) {
    CHECK(rconnect_host_ok("nas.local") && rconnect_host_ok("10.0.0.1"));
    CHECK(!rconnect_host_ok("-oProxy") && !rconnect_host_ok("a b") && !rconnect_host_ok("a\nhost = x") && !rconnect_host_ok(""));
    CHECK(rconnect_user_ok("ana.b") && rconnect_user_ok("") && !rconnect_user_ok("-x") && !rconnect_user_ok("a\nb"));
    CHECK(rconnect_path_ok("") && rconnect_path_ok("/dav/files") && !rconnect_path_ok("/a/../b") && !rconnect_path_ok("/..") && !rconnect_path_ok("a") && !rconnect_path_ok("/a\nb"));
    char c[600];
    CHECK(rconnect_config(c, sizeof c, "sftp", "h", 0, "u", "x", "", "/k") > 0 && strstr(c, "port = 22") && strstr(c, "known_hosts_file = /k"));
    CHECK(rconnect_config(c, sizeof c, "sftp", "h", 0, "u", "x", "", "") < 0);          /* never without a host key */
    CHECK(rconnect_config(c, sizeof c, "sftp", "h", 0, "u", "x", "", "/k") > 0 && strstr(c, "shell_type = none"));
    CHECK(rconnect_config(c, sizeof c, "ftps", "h", 0, "", "x", "", "") > 0 && strstr(c, "explicit_tls = true") && strstr(c, "anonymous") && !strstr(c, "no_check"));
    CHECK(rconnect_config(c, sizeof c, "davs", "h", 8443, "u", "x", "/d", "") > 0 && strstr(c, "url = https://h:8443/d") && !strstr(c, "no_check"));
    CHECK(rconnect_config(c, sizeof c, "smb", "h", 0, "u", "x", "", "") < 0);
    CHECK(rconnect_config(c, 8, "sftp", "h", 0, "u", "x", "", "/k") < 0);
    CHECK(rconnect_config(c, sizeof c, "sftp", "h\nkey_file = /x", 0, "u", "x", "", "/k") < 0);
    CHECK(rconnect_config(c, sizeof c, "sftp", "h", 0, "u\r", "x", "", "/k") < 0);
    CHECK(rconnect_config(c, sizeof c, "sftp", "h", 0, "u", "x\ny", "", "/k") < 0);
    CHECK(rconnect_config(c, sizeof c, "davs", "h", 0, "u", "x", "/a\tb", "") < 0);
    CHECK(rconnect_config(c, sizeof c, "ftps", "h", 990, "u", "x", "", "") > 0 && strstr(c, "tls = true") && !strstr(c, "explicit"));
    char tag[64];
    rconnect_host_tag("h", 2222, tag, sizeof tag); CHECK(!strcmp(tag, "[h]:2222"));
    rconnect_host_tag("h", 0, tag, sizeof tag); CHECK(!strcmp(tag, "h"));
    const char *k1 = "h ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJqm0000000000000000000000000000000000000000", *k2 = "h ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJqm1111111111111111111111111111111111111111";
    CHECK(rconnect_keyline_ok(k1, "h") && !rconnect_keyline_ok(k1, "other"));
    CHECK(!rconnect_keyline_ok("h ssh-ed25519 short", "h") && !rconnect_keyline_ok("h ssh-rsa AAAA;rm -rf /AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "h"));
    char f[300];
    snprintf(f, sizeof f, "x y z\n%s\n", k1);
    CHECK(rconnect_known(f, "h", k1) == 1);
    CHECK(rconnect_known(f, "h", k2) == 0);             /* a changed key */
    CHECK(rconnect_known(f, "new", "new ssh-rsa AAA") == -1);
    char pem[200];
    CHECK(rconnect_tls_parse("a\n-----BEGIN CERTIFICATE-----\nAB\n-----END CERTIFICATE-----\nVerify return code: 0 (ok)\n", pem, sizeof pem) == 1 && strstr(pem, "AB"));
    CHECK(rconnect_tls_parse("-----BEGIN CERTIFICATE-----\nAB\n-----END CERTIFICATE-----\nVerify return code: 18 (self-signed certificate)\n", pem, sizeof pem) == 0);
    CHECK(rconnect_tls_parse("connect: refused", pem, sizeof pem) == -1);
    CHECK(rconnect_tls_parse("-----BEGIN CERTIFICATE-----\nAB\n-----END CERTIFICATE-----\n", pem, 8) == -1);
}

static int req(const char *s, ShareReq *r) { static char b[1024]; snprintf(b, sizeof b, "%s", s); return share_req_parse(b, r); }

static void t_share(void) {
    ShareReq r;
    CHECK(!req("smb\n192.168.1.5\nPublic\nana\n\nse,cr=et", &r) && !strcmp(r.share, "Public") && !strcmp(r.user, "ana") && !strcmp(r.password, "se,cr=et") && !r.nfs);
    CHECK(!req("smb\n10.0.0.2\nMy Files\n\n\n", &r) && !r.user[0] && !r.password[0]);
    CHECK(!req("nfs\n172.16.1.1\n/srv/data\n\n\n", &r) && r.nfs);
    CHECK(req("nfs\n172.16.1.1\n/srv/data\nana\n\n", &r));          /* no credentials on NFS */
    CHECK(req("nfs\n172.16.1.1\n/srv/data\n\n\nx", &r));
    CHECK(req("nfs\n172.16.1.1\n/srv/../etc\n\n\n", &r));
    CHECK(req("nfs\n172.16.1.1\n/srv/..\n\n\n", &r));
    CHECK(req("nfs\n172.16.1.1\nsrv\n\n\n", &r));
    CHECK(req("nfs\n172.16.1.1\n/a,b:c\n\n\n", &r));
    CHECK(req("nfs\n172.16.1.1\n/a'b\n\n\n", &r));
    CHECK(req("smb\n8.8.8.8\nPublic\nana\n\nx", &r));               /* never a public address */
    CHECK(req("smb\n127.0.0.1\nPublic\nana\n\nx", &r));
    CHECK(req("smb\nfile.example.org\nPublic\nana\n\nx", &r));      /* never a name */
    CHECK(req("smb\n192.168.1.5\n../x\nana\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\n..\nana\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\na/b\nana\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\na\\b\nana\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\n\nana\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\nPublic\na,sec=none\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\nPublic\nana\n\nline\nbreak", &r));
    CHECK(req("ftp\n192.168.1.5\nPublic\nana\n\nx", &r));
    CHECK(req("smb\n192.168.1.5\nPublic", &r));
    char leaf[220];
    req("smb\n192.168.1.5\nPublic\n\n\n", &r); share_leaf(&r, leaf, sizeof leaf);
    CHECK(!strcmp(leaf, "Public on 192.168.1.5"));
    req("nfs\n192.168.1.5\n/srv/data\n\n\n", &r); share_leaf(&r, leaf, sizeof leaf);
    CHECK(!strcmp(leaf, "data on 192.168.1.5"));
    req("nfs\n192.168.1.5\n/\n\n\n", &r); share_leaf(&r, leaf, sizeof leaf);
    CHECK(!strcmp(leaf, "export on 192.168.1.5"));
    req("smb\n192.168.1.5\n.hidden\n\n\n", &r); share_leaf(&r, leaf, sizeof leaf);
    CHECK(leaf[0] != '.');
    CHECK(!share_path_ok("/media/ana/Public on 192.168.1.5", "ana"));
    CHECK(share_path_ok("/media/ana/a/b", "ana") && share_path_ok("/media/ana/", "ana") && share_path_ok("/media/bob/x", "ana") && share_path_ok("/etc", "ana") && share_path_ok("/media/ana/..", "ana"));
    CHECK(share_fstype("cifs") && share_fstype("nfs4") && !share_fstype("ext4") && !share_fstype("overlay"));
}

/* ---- ONC RPC ---- */
static size_t rpc_ok(uint8_t *b, uint32_t xid) { memset(b, 0, 24); b32(b, xid); b32(b + 4, 1); return 24; }

static void t_rpc(void) {
    uint8_t q[128], b[2048];
    size_t n = rpc_portmap_call(q, sizeof q, 7);
    CHECK(n == 60 && q[0] == 0x80 && q[3] == 56);
    CHECK(rpc_export_call(q, sizeof q, 7) == 44);
    CHECK(rpc_portmap_call(q, 10, 7) == 0);
    n = rpc_ok(b, 7);
    b32(b + n, 20048); n += 4;
    CHECK(rpc_portmap_reply(b, n, 7) == 20048);
    CHECK(rpc_portmap_reply(b, n, 8) == -1);                        /* another call's reply */
    CHECK(rpc_portmap_reply(b, n - 1, 7) == -1);
    b32(b + n - 4, 70000);
    CHECK(rpc_portmap_reply(b, n, 7) == -1);
    b32(b + 8, 1);                                                  /* rejected */
    b32(b + n - 4, 2049);
    CHECK(rpc_portmap_reply(b, n, 7) == -1);
    LanExport ex[4];
    n = rpc_ok(b, 9);
    b32(b + n, 1); b32(b + n + 4, 9); memcpy(b + n + 8, "/srv/data", 9); memset(b + n + 17, 0, 3); n += 20;
    b32(b + n, 1); b32(b + n + 4, 1); b[n + 8] = '*'; memset(b + n + 9, 0, 3); n += 12;
    b32(b + n, 0); n += 4;
    b32(b + n, 1); b32(b + n + 4, 4); memcpy(b + n + 8, "/lan", 4); n += 12;
    b32(b + n, 1); b32(b + n + 4, 10); memcpy(b + n + 8, "10.0.0.0/8\0\0", 12); n += 20;
    b32(b + n, 0); n += 4;
    b32(b + n, 0); n += 4;
    CHECK(rpc_export_reply(b, n, 9, ex, 4) == 2 && !strcmp(ex[0].path, "/srv/data") && ex[0].open == 1 && !strcmp(ex[1].path, "/lan") && ex[1].open == 0);
    CHECK(rpc_export_reply(b, n, 9, ex, 1) == 1);                   /* cap */
    for (size_t l = 0; l < n; l++) CHECK(rpc_export_reply(b, l, 9, ex, 4) == -1);
    /* a length past the buffer */
    n = rpc_ok(b, 9);
    b32(b + n, 1); b32(b + n + 4, 0xfffffff0u); n += 8;
    CHECK(rpc_export_reply(b, n, 9, ex, 4) == -1);
}

/* ---- SMB ---- */
static size_t smb_msg(uint8_t *b, uint16_t cmd, uint32_t status) {
    memset(b, 0, SMB_HDR);
    memcpy(b, "\xfeSMB", 4);
    p16(b + 4, SMB_HDR);
    p32(b + 8, status);
    p16(b + 12, cmd);
    return SMB_HDR;
}

static size_t challenge(uint8_t *b) {
    memset(b, 0, 48);
    memcpy(b, "NTLMSSP", 8);
    p32(b + 8, 2);
    uint8_t *ti = b + 48;
    size_t o = 0;
    p16(ti, 2); p16(ti + 2, 8); memcpy(ti + 4, "W\0O\0R\0K\0", 8); o += 12;
    p16(ti + o, 1); p16(ti + o + 2, 6); memcpy(ti + o + 4, "P\0I\0E\0", 6); o += 10;
    p16(ti + o, 0); p16(ti + o + 2, 0); o += 4;
    p16(b + 40, (uint32_t)o); p16(b + 42, (uint32_t)o); p32(b + 44, 48);
    return 48 + o;
}

static size_t ndr_s(uint8_t *o, const char *s) {
    size_t l = strlen(s) + 1, t = 12;
    p32(o, (uint32_t)l); p32(o + 4, 0); p32(o + 8, (uint32_t)l);
    for (size_t i = 0; i < l; i++) { o[t++] = (uint8_t)s[i]; o[t++] = 0; }
    while (t & 3) o[t++] = 0;
    return t;
}

static size_t share_reply(uint8_t *b) {
    static const struct { const char *n, *c; uint32_t t; } e[] = { { "Public", "Open to all", 0 }, { "print$", "Drivers", 0 }, { "IPC$", "IPC", 0x80000003u }, { "C$", "Admin", 0x80000000u }, { "laser", "Printer", 1 }, { "Datos", "", 0 } };
    memset(b, 0, 24);
    b[0] = 5; b[2] = 2;
    size_t o = 24;
    p32(b + o, 1); p32(b + o + 4, 1); p32(b + o + 8, 0x20000); p32(b + o + 12, 6); p32(b + o + 16, 0x20004); p32(b + o + 20, 6);
    o += 24;
    for (int i = 0; i < 6; i++) { p32(b + o, 0x20008 + (uint32_t)i); p32(b + o + 4, e[i].t); p32(b + o + 8, 0x30000 + (uint32_t)i); o += 12; }
    for (int i = 0; i < 6; i++) { o += ndr_s(b + o, e[i].n); o += ndr_s(b + o, e[i].c); }
    p32(b + o, 6); p32(b + o + 4, 0); p32(b + o + 8, 0);
    return o + 12;
}

static void t_smb(void) {
    uint8_t out[1024], b[4096];
    SmbMsg m;
    size_t n = smb_negotiate(out, sizeof out, 0);
    CHECK(n > 100 && !memcmp(out + 4, "\xfeSMB", 4) && out[3] == n - 4);
    CHECK(!smb_parse(out + 4, n - 4, &m) && m.cmd == 0);
    CHECK(smb_negotiate(out, 20, 0) == 0);
    CHECK(smb_parse(out, 10, &m) && smb_parse((const uint8_t *)"\xffSMB0000000000000000000000000000000000000000000000000000000000000000", 64, &m));
    n = smb_msg(b, 0, 0);
    memset(b + n, 0, 65); p16(b + n, 65); p16(b + n + 2, 3); p16(b + n + 4, 0x0302); n += 65;
    int sign = 0;
    CHECK(!smb_parse(b, n, &m) && smb_negotiate_reply(&m, &sign) == 0x0302 && sign == 1);
    n = smb_msg(b, 1, SMB_STATUS_MORE);
    memset(b + n, 0, 8); p16(b + n, 9); p16(b + n + 4, SMB_HDR + 8);
    size_t cl = challenge(b + n + 8);
    p16(b + n + 6, (uint32_t)cl); n += 8 + cl;
    const uint8_t *blob; size_t bl; unsigned fl;
    char name[64], dom[64];
    CHECK(!smb_parse(b, n, &m) && !smb_session_reply(&m, &blob, &bl, &fl) && bl == cl);
    CHECK(!ntlm_challenge_names(blob, bl, name, sizeof name, dom, sizeof dom) && !strcmp(name, "PIE") && !strcmp(dom, "WORK"));
    for (size_t l = 0; l < cl; l++) { char a[64], c[64]; ntlm_challenge_names(blob, l, a, sizeof a, c, sizeof c); }
    p16(b + SMB_HDR + 6, 60000);                                    /* the blob claims more than arrived */
    CHECK(smb_session_reply(&m, &blob, &bl, &fl) == -1);
    uint8_t a3[128];
    CHECK(ntlm_anonymous_auth(a3, sizeof a3) == 65 && a3[8] == 3 && a3[12] == 1);
    CHECK(ntlm_negotiate(a3, sizeof a3) == 32 && a3[8] == 1);
    /* IOCTL: the output's bounds */
    n = smb_msg(b, 11, 0);
    memset(b + n, 0, 48); p16(b + n, 49); p32(b + n + 32, SMB_HDR + 48); p32(b + n + 36, 4); n += 48;
    memcpy(b + n, "abcd", 4); n += 4;
    const uint8_t *o; size_t ol;
    CHECK(!smb_parse(b, n, &m) && !smb_ioctl_reply(&m, &o, &ol) && ol == 4 && !memcmp(o, "abcd", 4));
    p32(b + SMB_HDR + 36, 0xffffffffu);
    CHECK(smb_ioctl_reply(&m, &o, &ol) == -1);
    p32(b + SMB_HDR + 36, 4); p32(b + SMB_HDR + 32, 0xfffffff0u);
    CHECK(smb_ioctl_reply(&m, &o, &ol) == -1);
    /* DCE/RPC */
    uint8_t d[512];
    CHECK(dce_bind(d, sizeof d) == 72 && d[2] == 11 && d[8] == 72);
    size_t q = dce_share_enum(d, sizeof d, "192.168.100.200");
    CHECK(q > 80 && d[2] == 0 && d[22] == 15 && d[8] == q);
    uint8_t ack[64];
    memset(ack, 0, sizeof ack);
    ack[0] = 5; ack[2] = 12; p16(ack + 24, 4); memcpy(ack + 26, "135", 4); ack[32] = 1;
    CHECK(!dce_bind_ack(ack, 60));
    p16(ack + 36, 2);
    CHECK(dce_bind_ack(ack, 60) == -1);
    p16(ack + 36, 0);
    CHECK(dce_bind_ack(ack, 20) == -1);
    LanShare sh[8];
    n = share_reply(b);
    int c = dce_share_reply(b, n, sh, 8);
    CHECK(c == 2 && !strcmp(sh[0].name, "Public") && !strcmp(sh[0].comment, "Open to all") && !strcmp(sh[1].name, "Datos"));  /* no $ names, no printers */
    for (size_t l = 0; l + 12 < n; l++) dce_share_reply(b, l, sh, 8);
    p32(b + 24 + 12, 0xffffffffu);
    CHECK(dce_share_reply(b, n, sh, 8) == -1);
}

/* ---- mDNS: our own query reads back with the same parser ---- */
static void t_mdns(void) {
    uint8_t q[1200];
    const char *names[LAN_SERVICES];
    int types[LAN_SERVICES];
    static char full[LAN_SERVICES][40];
    for (int i = 0; i < LAN_SERVICES; i++) { snprintf(full[i], sizeof full[i], "%s.local", lan_services[i].type); names[i] = full[i]; types[i] = MDNS_T_PTR; }
    size_t n = mdns_query(q, sizeof q, names, types, LAN_SERVICES, 1);
    CHECK(n > 12 && n < 1200 && q[5] == LAN_SERVICES);
    MdnsCache *c = calloc(1, sizeof *c);
    /* a compression loop, a pointer past the end, a truncated header */
    uint8_t bad[64] = { 0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xc0, 12, 0, 12, 0, 1, 0, 0, 0, 1, 0, 2, 0xc0, 12 };
    CHECK(mdns_parse(bad, 26, c) == -1);
    bad[13] = 200;
    CHECK(mdns_parse(bad, 26, c) == -1);
    CHECK(mdns_parse(bad, 5, c) == -1);
    free(c);
}

/* ---- every parser over damaged copies of valid answers: must never read out of bounds ---- */
static uint32_t get_xid(const uint8_t *h, size_t l) { return l >= 4 ? (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3] : 0; }
static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

static void t_fuzz(void) {
    static uint8_t base[4096], b[4096];
    size_t bl = share_reply(base);
    LanShare sh[8]; LanExport ex[8];
    char a[64], c[64];
    MdnsCache *mc = calloc(1, sizeof *mc);
    for (int i = 0; i < 40000; i++) {
        size_t l = rnd() % (bl + 1);
        memcpy(b, base, bl);
        for (int k = rnd() % 12; k > 0; k--) b[rnd() % bl] = (uint8_t)rnd();
        uint8_t *h = malloc(l ? l : 1);                 /* exact allocation: ASan sees one byte too far */
        memcpy(h, b, l);
        dce_share_reply(h, l, sh, 8);
        dce_bind_ack(h, l);
        ntlm_challenge_names(h, l, a, sizeof a, c, sizeof c);
        rpc_export_reply(h, l, get_xid(h, l), ex, 8);
        rpc_portmap_reply(h, l, get_xid(h, l));
        SmbMsg m;
        if (!smb_parse(h, l, &m)) {
            const uint8_t *o; size_t ol; unsigned fl; uint8_t fid[16]; int s;
            smb_negotiate_reply(&m, &s);
            smb_session_reply(&m, &o, &ol, &fl);
            smb_create_reply(&m, fid);
            smb_ioctl_reply(&m, &o, &ol);
        }
        mdns_parse(h, l, mc);
        if (mc->nptr > 200) memset(mc, 0, sizeof *mc);
        free(h);
    }
    free(mc);
    pass++;
}

int main(void) {
    t_addr();
    t_smbauth();
    t_rconnect();
    t_review();
    t_share();
    t_rpc();
    t_smb();
    t_mdns();
    t_fuzz();
    printf("lan-test: %d passed, %d failed\n", pass, failc);
    return failc ? 1 : 0;
}
