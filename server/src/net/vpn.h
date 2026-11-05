/* vpn.h — common pieces of IPsec, PPTP, OpenConnect and Fortinet (see vpn.c). */
#ifndef LORD_NET_VPN_H
#define LORD_NET_VPN_H

#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include "netpriv.h"

#define VPN_MAX_KV 32
#define VPN_MAX_PEM 4

typedef struct {
    char key[VPN_MAX_KV][24], val[VPN_MAX_KV][256];
    int n;
    char pem_key[VPN_MAX_PEM][16];
    char *pem[VPN_MAX_PEM];
    int npem;
} VpnKv;

int  vpn_kv_parse(const char *text, const char *const *keys, const char *const *pem_keys, VpnKv *kv, char *err, size_t errn);
const char *vpn_kv(const VpnKv *kv, const char *key);
const char *vpn_kv_pem(const VpnKv *kv, const char *key);
void vpn_kv_free(VpnKv *kv);
int  vpn_kv_save(const char *name, const char *type, const VpnKv *kv);
int  vpn_kv_load(const char *name, const char *type, VpnKv *kv);
int  vpn_meta_save(const char *name, const char *json);
int  vpn_text_ok(const char *s, size_t max);    /* no control characters */
int  vpn_host_ok(const char *s);                /* name or IP (with :port or [v6]) */
int  vpn_routes_valid(const char *list);

int  vpn_resolve(const char *host, int *family, uint8_t a[16]);
int  vpn_routes_full(const char *ifn, int sfam, const uint8_t server[16]);
int  vpn_routes_list(const char *ifn, const char *list);
void vpn_routes_clear(int sfam, const uint8_t server[16]);

extern volatile sig_atomic_t vpn_stop;
void vpn_signals(void);
pid_t vpn_spawn(const char *const argv[], int *out_fd, int *in_fd, const char *extra_env);   /* in_fd: NULL = /dev/null */
void vpn_reap(pid_t pid);
void vpn_mark(const char *ifn, const char *name, int set);
int  vpn_status_json(const char *name, const char *type, Buf *out);

int  vpn_type_known(const char *type);
int  vpn_type_available(const char *type);
const char *vpn_type_package(const char *type);

/* Each type */
int  ipsec_import(const char *name, const char *text, char *err, size_t errn);
void ipsec_run(const char *name);
int  pptp_import(const char *name, const char *text, char *err, size_t errn);
void pptp_run(const char *name);
int  oc_import(const char *name, const char *text, char *err, size_t errn);
void oc_run(const char *name);
int  forti_import(const char *name, const char *text, char *err, size_t errn);
void forti_run(const char *name);
int  forti_trust(const char *name, const char *sha256, const char *host, char *err, size_t errn);

#endif
