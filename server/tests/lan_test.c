/* lan_test.c — Files → Network without a network: the address policy, the arguments of the share-*
 * actions, and every parser of lord-lan against crafted and damaged answers. Built with ASan and UBSan. */
#include "../src/lan/lanaddr.h"
#include "../src/lan/lanparse.h"
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
    t_share();
    t_rpc();
    t_smb();
    t_mdns();
    t_fuzz();
    printf("lan-test: %d passed, %d failed\n", pass, failc);
    return failc ? 1 : 0;
}
