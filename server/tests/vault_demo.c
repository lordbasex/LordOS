/* vault_demo.c — builds a demo vault (for the screenshots in docs/PASSWORDS.md) with
 * entries from every template, a tree of user categories, tags, 2FA, attachments and
 * weak, reused and old passwords for the report. All invented.
 *
 *   build/vault-demo <password> [image.jpg]      (HOME = the user's; launches its own axia-vault)
 * It does not overwrite an existing vault. */
#include "vault/vclient.h"
#include "vault/vjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

static char last[1 << 20];

static const char *req(const char *json, int *fds, int nfds) {
    int id = vc_request(json, strlen(json), fds, nfds);
    for (int tries = 0; id > 0 && tries < 30000; tries++) {
        char *s = vc_poll();
        if (!s) { usleep(2000); continue; }
        Vj j;
        int found = 0;
        if (!vj_parse(&j, s, strlen(s))) {
            for (int i = 0, k = 1; i < j.t[0].count; i++, k = j.t[k].next) {
                int d = vj_get(&j, k, "data");
                if ((int)vj_getn(&j, k, "id", -1) == id && vj_streq(&j, vj_get(&j, k, "kind"), "rep") && d >= 0) {
                    size_t n = j.t[d].end - j.t[d].start;
                    if (n >= sizeof last) n = sizeof last - 1;
                    memcpy(last, j.s + j.t[d].start, n);
                    last[n] = 0;
                    found = 1;
                }
            }
            vj_free(&j);
        }
        vc_poll_free(s);
        if (found) return last;
    }
    return "{\"error\":\"timeout\"}";
}

static char lastid[40];
static void save(const char *entry) {
    char buf[8192];
    snprintf(buf, sizeof buf, "{\"op\":\"save\",\"entry\":%s}", entry);
    const char *r = req(buf, NULL, 0);
    char *p = strstr(r, "\"id\":\"");
    if (p) { memcpy(lastid, p + 6, 32); lastid[32] = 0; }
    if (!strstr(r, "\"ok\":true")) fprintf(stderr, "error: %s\n", r);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vault-demo <password> [image]\n"); return 2; }
    setenv("XDG_RUNTIME_DIR", "/nonexistent", 1);
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    if (vc_start()) { fprintf(stderr, "could not launch axia-vault\n"); return 1; }
    char buf[4096];
    snprintf(buf, sizeof buf, "{\"op\":\"create\",\"method\":\"password\",\"password\":\"%s\"}", argv[1]);
    const char *r = req(buf, NULL, 0);
    printf("create: %s\n", r);
    if (!strstr(r, "\"ok\":true")) return 1;
    long now = (long)time(NULL), old = now - 400L * 86400, mid = now - 120L * 86400;
    req("{\"op\":\"set-cats\",\"cats\":[{\"id\":\"utrabajo\",\"name\":\"Trabajo\",\"icon\":\"💼\",\"color\":\"#5e5ce6\",\"template\":\"internet\"},"
        "{\"id\":\"uclientes\",\"name\":\"Clientes\",\"icon\":\"📁\",\"color\":\"#bf5af2\",\"template\":\"internet\",\"parent\":\"utrabajo\"},"
        "{\"id\":\"uhogar\",\"name\":\"Hogar\",\"icon\":\"🏠\",\"color\":\"#30d158\",\"template\":\"general\"},"
        "{\"id\":\"uinventario\",\"name\":\"Inventario\",\"icon\":\"📦\",\"color\":\"#ff9f0a\",\"template\":\"general\",\"parent\":\"uhogar\","
        "\"fields\":[{\"k\":\"c1\",\"t\":\"text\",\"l\":\"Número de serie\"},{\"k\":\"c2\",\"t\":\"date\",\"l\":\"Garantía hasta\"},{\"k\":\"c3\",\"t\":\"number\",\"l\":\"Precio\"}]}]}", NULL, 0);
    req("{\"op\":\"set-tags\",\"tags\":[{\"name\":\"trabajo\",\"color\":\"#5e5ce6\"},{\"name\":\"familia\",\"color\":\"#ff375f\"},{\"name\":\"finanzas\",\"color\":\"#30d158\"},{\"name\":\"viajes\",\"color\":\"#ff9f0a\"}]}", NULL, 0);

    save("{\"cat\":\"internet\",\"title\":\"Correo personal\",\"fav\":true,\"tags\":[\"familia\"],\"rotate\":90,\"f\":[{\"k\":\"url\",\"t\":\"url\",\"v\":\"https://correo.example.com\"},"
         "{\"k\":\"username\",\"t\":\"text\",\"v\":\"ana.gomez@example.com\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"Ql9#vT2m!xR7kLp4\"},"
         "{\"k\":\"totp\",\"t\":\"totp\",\"v\":\"otpauth://totp/Correo:ana.gomez@example.com?secret=JBSWY3DPEHPK3PXP&issuer=Correo\"}],\"notes\":\"Cuenta principal. **No** compartir.\"}");
    save("{\"cat\":\"internet\",\"title\":\"Tienda en línea\",\"tags\":[\"familia\"],\"f\":[{\"k\":\"url\",\"t\":\"url\",\"v\":\"https://tienda.example.net\"},"
         "{\"k\":\"username\",\"t\":\"text\",\"v\":\"ana.gomez\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"password1\"}]}");
    snprintf(buf, sizeof buf, "{\"cat\":\"internet\",\"title\":\"Foro de fotografía\",\"created\":%ld,\"modified\":%ld,\"f\":[{\"k\":\"url\",\"t\":\"url\",\"v\":\"https://foro.example.org\"},"
             "{\"k\":\"username\",\"t\":\"text\",\"v\":\"anag\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"Ql9#vT2m!xR7kLp4\"}]}", old, old);
    save(buf);
    save("{\"cat\":\"email\",\"title\":\"Correo del trabajo\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"email\",\"t\":\"email\",\"v\":\"agomez@empresa.example\"},"
         "{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"Vx8$wq2Lm#9Tz1Ke\"},{\"k\":\"imap_host\",\"t\":\"text\",\"v\":\"imap.empresa.example\"},{\"k\":\"imap_port\",\"t\":\"number\",\"v\":\"993\"},"
         "{\"k\":\"smtp_host\",\"t\":\"text\",\"v\":\"smtp.empresa.example\"},{\"k\":\"smtp_port\",\"t\":\"number\",\"v\":\"587\"},{\"k\":\"app_password\",\"t\":\"hidden\",\"v\":\"abcd efgh ijkl mnop\"}]}");
    snprintf(buf, sizeof buf, "{\"cat\":\"finance\",\"title\":\"Banco del Sur\",\"fav\":true,\"tags\":[\"finanzas\"],\"rotate\":90,\"created\":%ld,\"modified\":%ld,"
             "\"f\":[{\"k\":\"bank\",\"t\":\"text\",\"v\":\"Banco del Sur\"},{\"k\":\"client\",\"t\":\"text\",\"v\":\"00451287\"},{\"k\":\"username\",\"t\":\"text\",\"v\":\"agomez\"},"
             "{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"Pr0teg!da-2025\"},{\"k\":\"transfer_key\",\"t\":\"hidden\",\"v\":\"918273\"},{\"k\":\"pin\",\"t\":\"pin\",\"v\":\"4821\"},"
             "{\"k\":\"coords\",\"t\":\"hidden\",\"v\":\"A1 37  A2 81  A3 05\\nB1 64  B2 12  B3 99\"}]}", mid, mid);
    save(buf);
    save("{\"cat\":\"cards\",\"title\":\"Tarjeta de crédito\",\"tags\":[\"finanzas\",\"viajes\"],\"f\":[{\"k\":\"holder\",\"t\":\"text\",\"v\":\"ANA GOMEZ\"},"
         "{\"k\":\"number\",\"t\":\"hidden\",\"v\":\"4000 1234 5678 9010\"},{\"k\":\"expiry\",\"t\":\"text\",\"v\":\"08/29\"},{\"k\":\"cvv\",\"t\":\"pin\",\"v\":\"321\"},"
         "{\"k\":\"pin\",\"t\":\"pin\",\"v\":\"1357\"},{\"k\":\"phone\",\"t\":\"text\",\"v\":\"0800-555-0100\"}]}");
    save("{\"cat\":\"wifi\",\"title\":\"Wi-Fi de casa\",\"fav\":true,\"tags\":[\"familia\"],\"f\":[{\"k\":\"ssid\",\"t\":\"text\",\"v\":\"Casa-5G\"},"
         "{\"k\":\"security\",\"t\":\"text\",\"v\":\"WPA2\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"girasol-tranvia-41\"},{\"k\":\"hidden\",\"t\":\"bool\",\"v\":\"false\"}]}");
    save("{\"cat\":\"wifi\",\"title\":\"Wi-Fi de la oficina\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"ssid\",\"t\":\"text\",\"v\":\"Oficina\"},"
         "{\"k\":\"security\",\"t\":\"text\",\"v\":\"WPA3\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"K7#pQ2!vX9mL\"}]}");
    save("{\"cat\":\"network\",\"title\":\"Router\",\"f\":[{\"k\":\"device\",\"t\":\"text\",\"v\":\"Router del living\"},{\"k\":\"host\",\"t\":\"text\",\"v\":\"192.168.1.1\"},"
         "{\"k\":\"port\",\"t\":\"number\",\"v\":\"443\"},{\"k\":\"username\",\"t\":\"text\",\"v\":\"admin\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"admin\"}]}");
    save("{\"cat\":\"ssh\",\"title\":\"Servidor de backups\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"host\",\"t\":\"text\",\"v\":\"backup.empresa.example\"},"
         "{\"k\":\"port\",\"t\":\"number\",\"v\":\"22\"},{\"k\":\"username\",\"t\":\"text\",\"v\":\"respaldo\"},{\"k\":\"private_key\",\"t\":\"hidden\",\"v\":\"-----BEGIN OPENSSH PRIVATE KEY-----\\nb3BlbnNzaC1rZXktdjEAAAAA…(clave de ejemplo)\\n-----END OPENSSH PRIVATE KEY-----\"},"
         "{\"k\":\"fingerprint\",\"t\":\"text\",\"v\":\"SHA256:Xq3P9ZtE0bWn5yJ2vH8kLcR1aM4dF6gS7uT0oQiNe8\"}]}");
    save("{\"cat\":\"database\",\"title\":\"Base de clientes\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"engine\",\"t\":\"text\",\"v\":\"PostgreSQL\"},"
         "{\"k\":\"host\",\"t\":\"text\",\"v\":\"db.empresa.example\"},{\"k\":\"port\",\"t\":\"number\",\"v\":\"5432\"},{\"k\":\"database\",\"t\":\"text\",\"v\":\"clientes\"},"
         "{\"k\":\"username\",\"t\":\"text\",\"v\":\"app_lectura\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"Vx8$wq2Lm#9Tz1Ke\"}]}");
    save("{\"cat\":\"apps\",\"title\":\"Mensajería\",\"f\":[{\"k\":\"app\",\"t\":\"text\",\"v\":\"Mensajería\"},{\"k\":\"account\",\"t\":\"text\",\"v\":\"+54 11 5555-0100\"},"
         "{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"8xT!mN3qR6wZ\"},{\"k\":\"totp\",\"t\":\"totp\",\"v\":\"otpauth://totp/Mensajeria:ana?secret=KRSXG5CTMVRXEZLUKN2XAZLSKNSWG4TFOQ&issuer=Mensajer%C3%ADa&digits=6\"},"
         "{\"k\":\"recovery_codes\",\"t\":\"hidden\",\"v\":\"8f2a-11c0\\n9b3e-77d1\\n2c4f-0a9e\"}]}");
    save("{\"cat\":\"licenses\",\"title\":\"Editor de fotos\",\"f\":[{\"k\":\"product\",\"t\":\"text\",\"v\":\"Editor de fotos Pro\"},{\"k\":\"version\",\"t\":\"text\",\"v\":\"12\"},"
         "{\"k\":\"license_key\",\"t\":\"hidden\",\"v\":\"EFP-7Q2K-9M4X-RT8B-1ZC6\"},{\"k\":\"email\",\"t\":\"email\",\"v\":\"ana.gomez@example.com\"},"
         "{\"k\":\"purchase_date\",\"t\":\"date\",\"v\":\"2025-03-14\"},{\"k\":\"expiry\",\"t\":\"date\",\"v\":\"2027-03-14\"}]}");
    char lic[40];
    snprintf(lic, sizeof lic, "%s", lastid);
    save("{\"cat\":\"api\",\"title\":\"API de mapas\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"service\",\"t\":\"text\",\"v\":\"Mapas\"},{\"k\":\"key\",\"t\":\"hidden\",\"v\":\"mk_live_51Hx…ejemplo\"},"
         "{\"k\":\"secret\",\"t\":\"hidden\",\"v\":\"ms_9f8e7d…ejemplo\"},{\"k\":\"expiry\",\"t\":\"date\",\"v\":\"2027-01-01\"},{\"k\":\"scopes\",\"t\":\"text\",\"v\":\"geocode, rutas\"}]}");
    save("{\"cat\":\"identity\",\"title\":\"DNI\",\"tags\":[\"viajes\"],\"f\":[{\"k\":\"doc_type\",\"t\":\"text\",\"v\":\"dni\"},{\"k\":\"number\",\"t\":\"text\",\"v\":\"30.123.456\"},"
         "{\"k\":\"holder\",\"t\":\"text\",\"v\":\"Ana Gómez\"},{\"k\":\"issued\",\"t\":\"date\",\"v\":\"2019-05-02\"},{\"k\":\"expiry\",\"t\":\"date\",\"v\":\"2034-05-02\"},{\"k\":\"issuer\",\"t\":\"text\",\"v\":\"Registro Nacional\"}]}");
    char dni[40];
    snprintf(dni, sizeof dni, "%s", lastid);
    save("{\"cat\":\"identity\",\"title\":\"Pasaporte\",\"tags\":[\"viajes\"],\"f\":[{\"k\":\"doc_type\",\"t\":\"text\",\"v\":\"passport\"},{\"k\":\"number\",\"t\":\"text\",\"v\":\"AAB123456\"},"
         "{\"k\":\"holder\",\"t\":\"text\",\"v\":\"Ana Gómez\"},{\"k\":\"expiry\",\"t\":\"date\",\"v\":\"2031-11-20\"}]}");
    save("{\"cat\":\"crypto\",\"title\":\"Billetera fría\",\"f\":[{\"k\":\"wallet\",\"t\":\"text\",\"v\":\"Billetera de hardware\"},{\"k\":\"address\",\"t\":\"text\",\"v\":\"bc1qexample0000000000000000000000000000\"},"
         "{\"k\":\"seed\",\"t\":\"hidden\",\"v\":\"palabra uno dos tres cuatro cinco seis siete ocho nueve diez once doce\"}]}");
    save("{\"cat\":\"vpn\",\"title\":\"VPN de la oficina\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"server\",\"t\":\"text\",\"v\":\"vpn.empresa.example\"},{\"k\":\"vpn_type\",\"t\":\"text\",\"v\":\"WireGuard\"},"
         "{\"k\":\"username\",\"t\":\"text\",\"v\":\"agomez\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"wG7!kd2#Lq9\"}]}");
    save("{\"cat\":\"notes\",\"title\":\"Caja fuerte\",\"fav\":false,\"notes\":\"# Caja fuerte\\n\\n- Combinación: **31-07-22**\\n- Está detrás del cuadro del pasillo\\n- Llave de repuesto: casa de Marta\"}");
    save("{\"cat\":\"uclientes\",\"title\":\"Portal de Cliente Norte\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"url\",\"t\":\"url\",\"v\":\"https://norte.example.com\"},"
         "{\"k\":\"username\",\"t\":\"text\",\"v\":\"agomez\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"Nt5#pL8!qW2z\"}]}");
    save("{\"cat\":\"utrabajo\",\"title\":\"Intranet\",\"tags\":[\"trabajo\"],\"f\":[{\"k\":\"url\",\"t\":\"url\",\"v\":\"https://intranet.empresa.example\"},"
         "{\"k\":\"username\",\"t\":\"text\",\"v\":\"agomez\"},{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"empresa2024\"}]}");
    save("{\"cat\":\"uinventario\",\"title\":\"Notebook\",\"f\":[{\"k\":\"c1\",\"t\":\"text\",\"l\":\"Número de serie\",\"v\":\"NB-55X-0192\"},{\"k\":\"c2\",\"t\":\"date\",\"l\":\"Garantía hasta\",\"v\":\"2027-02-01\"},"
         "{\"k\":\"c3\",\"t\":\"number\",\"l\":\"Precio\",\"v\":\"1200\"}]}");
    save("{\"cat\":\"general\",\"title\":\"Alarma de la casa\",\"arch\":true,\"f\":[{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"1234\"}]}");
    save("{\"cat\":\"internet\",\"title\":\"Red social vieja\",\"f\":[{\"k\":\"url\",\"t\":\"url\",\"v\":\"https://social.example\"},{\"k\":\"username\",\"t\":\"text\",\"v\":\"anita\"},"
         "{\"k\":\"password\",\"t\":\"hidden\",\"v\":\"qwerty\"}]}");
    snprintf(buf, sizeof buf, "{\"op\":\"trash\",\"id\":\"%s\"}", lastid);
    req(buf, NULL, 0);
    /* attachments: the license (text) and the photo of the ID document (if an image was given) */
    FILE *f = fopen("/tmp/licencia-demo.txt", "w");
    if (f) { fputs("Editor de fotos Pro 12\nLicencia: EFP-7Q2K-9M4X-RT8B-1ZC6\nTitular: Ana Gómez\n", f); fclose(f); }
    int fd = open("/tmp/licencia-demo.txt", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        snprintf(buf, sizeof buf, "{\"op\":\"att-add\",\"id\":\"%s\",\"name\":\"licencia.txt\",\"mime\":\"text/plain\"}", lic);
        printf("license attachment: %s\n", req(buf, &fd, 1));
    }
    unlink("/tmp/licencia-demo.txt");
    if (argc > 2 && (fd = open(argv[2], O_RDONLY | O_CLOEXEC)) >= 0) {
        snprintf(buf, sizeof buf, "{\"op\":\"att-add\",\"id\":\"%s\",\"name\":\"foto-documento.jpg\",\"mime\":\"image/jpeg\"}", dni);
        printf("photo attachment: %s\n", req(buf, &fd, 1));
    }
    printf("done: %s\n", req("{\"op\":\"status\"}", NULL, 0));
    return 0;
}
