/* ssh_root_test.c — root over SSH (src/remote/ssh_root.c): public key lines LordOS accepts or refuses,
 * fingerprints as ssh-keygen -l prints them, and the fixed drop-in text per mode.
 *
 *   build/ssh-root-test [keyfile.pub fingerprint]   (make -C server test runs it) */
#include "../src/remote/ssh_root.h"

#include <stdio.h>
#include <string.h>

/* stubs: the actions aren't run here, only the parsers */
void admin_say(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
int admin_run(const char *const argv[]) { (void)argv; return 0; }
int admin_run_input(const char *const argv[], const char *input) { (void)argv; (void)input; return 0; }
int users_check_password(const char *user, const char *pass) { (void)user; (void)pass; return 0; }

static int n, bad;
#define CHECK(name, cond) do { n++; if (!(cond)) { bad++; printf("✗ %s\n", name); } } while (0)

/* a well-formed ed25519 key line (the key bytes are 1..32); real keys are checked against ssh-keygen below */
#define ED "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g"

int main(int argc, char **argv) {
    SshKey k;
    CHECK("ed25519 with comment", sshkey_parse(ED " test@lordos", &k) == 0 && !strcmp(k.type, "ssh-ed25519") && !strcmp(k.comment, "test@lordos"));
    CHECK("ed25519 without comment, trailing newline", sshkey_parse(ED "\n", &k) == 0 && !k.comment[0]);
    CHECK("options are refused", sshkey_parse("command=\"/bin/sh\" " ED, &k) != 0);
    CHECK("two lines are refused", sshkey_parse(ED "\n" ED, &k) != 0);
    CHECK("unknown type", sshkey_parse("ssh-dss AAAAB3NzaC1kc3MAAACBAP", &k) != 0);
    CHECK("type mismatch with the blob", sshkey_parse("ssh-rsa AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g", &k) != 0);
    CHECK("bad base64", sshkey_parse("ssh-ed25519 AAAA$$$$", &k) != 0);
    CHECK("truncated blob", sshkey_parse("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5", &k) != 0);
    CHECK("control character in the comment", sshkey_parse(ED " bad\x01", &k) != 0);
    CHECK("comment too long", sshkey_parse(ED " xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", &k) != 0);
    CHECK("empty", sshkey_parse("", &k) != 0 && sshkey_parse(NULL, &k) != 0);
    const char *t;
    CHECK("off: no file", ssh_root_text("off", &t) == 0 && t == NULL);
    CHECK("key: prohibit-password", ssh_root_text("key", &t) == 0 && strstr(t, "PermitRootLogin prohibit-password\n"));
    CHECK("password: yes", ssh_root_text("password", &t) == 0 && strstr(t, "PermitRootLogin yes\n"));
    CHECK("anything else refused", ssh_root_text("yes", &t) != 0 && ssh_root_text("", &t) != 0);
    /* with a real key file and ssh-keygen's fingerprint (the VM run passes them) */
    if (argc == 3) {
        FILE *f = fopen(argv[1], "r");
        char line[4096] = "";
        if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
        char fp[60] = "";
        int ok = sshkey_parse(line, &k) == 0;
        if (ok) sshkey_fingerprint(&k, fp);
        CHECK("generated key parses", ok);
        CHECK("fingerprint matches ssh-keygen", !strcmp(fp, argv[2]));
        if (strcmp(fp, argv[2])) printf("  got %s want %s\n", fp, argv[2]);
    }
    printf("ssh-root-test: %d/%d ok\n", n - bad, n);
    return bad ? 1 : 0;
}
