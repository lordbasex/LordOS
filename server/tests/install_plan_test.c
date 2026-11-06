/* install_plan_test.c — install-run's plan parser: what must never become an erase by accident. It
 * includes install.c for the static parse_plan; the helper is stubbed, nothing here runs a program. */
#include "install/install.c"

void admin_say(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
int admin_run(const char *const argv[]) { (void)argv; return 1; }
int admin_run_capture(const char *const argv[], char *out, size_t cap) { (void)argv; (void)cap; out[0] = 0; return 1; }

static int fails, checks;
#define T(c) do { checks++; if (!(c)) { fails++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    static Plan p;
    /* the wizard's two plans */
    T(parse_plan("{\"mode\":\"erase\",\"disk\":\"sda\",\"swap\":2048}", &p) == 0 && fsp_mode(p.mode) == 0 && !strcmp(p.disk, "sda") && p.swap == 2048);
    T(parse_plan("{\"mode\":\"free\",\"disk\":\"sda\",\"freeStart\":41943040,\"freeEnd\":209713151}", &p) == 0 && fsp_mode(p.mode) == 1);
    T(p.free_start == 41943040 && p.free_end == 209713151);
    /* above 2^53 a number from JavaScript would be wrong anyway; here it is exact */
    T(parse_plan("{\"freeStart\":281474976710655}", &p) == 0 && p.free_start == 281474976710655ull);
    /* the old plan, without a mode, is an erase as it always was */
    T(parse_plan("{\"disk\":\"sda\"}", &p) == 0 && fsp_mode(p.mode) == 0);

    /* a mode that isn't a plain string doesn't fall through to erase: the plan is refused */
    T(parse_plan("{\"mode\":1,\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":true,\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":null,\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":{},\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":[\"free\"],\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mo\\u0064e\":1}", &p) != 0);
    T(parse_plan("{\"mode\":\"free\",\"mode\":\"erase\"}", &p) != 0);
    T(parse_plan("{\"mode\":\"erase\",\"mode\":\"free\"}", &p) != 0);
    T(parse_plan("{\"mode\":\"a-mode-much-too-long-for-its-field\"}", &p) != 0);
    /* an unknown mode parses and validate() refuses it first of all, before looking at any disk */
    T(parse_plan("{\"mode\":\"FREE\",\"disk\":\"sda\"}", &p) == 0);
    T(!strcmp(validate(&p), "install.badMode"));
    /* install-account is not a mode: on its own action it never reaches a disk, and here it is refused */
    T(parse_plan("{\"mode\":\"account\",\"disk\":\"sda\"}", &p) == 0);
    T(!strcmp(validate(&p), "install.badMode"));
    /* outside the OVA's first boot the action does nothing (no such marker on a test machine) */
    T(!is_first_account());
    /* bad region numbers */
    T(parse_plan("{\"mode\":\"free\",\"freeStart\":-5}", &p) != 0);
    T(parse_plan("{\"mode\":\"free\",\"freeEnd\":99999999999999999999999}", &p) != 0);

    /* the unattended install's fields: refused from the interface, taken only with install_set_unattended */
    static const char *hash = "$6$saltsalt$CnZ0Yx4vF1lW2bq8Qm1sT2uPq7mO0wQ2B6X1y9pC1hW3s5lKj8Gm0uTn2aY4eRz6iV8bN1cD3fH5jL7nP9rS0tU";
    T(parse_plan("{\"disk\":\"sda\",\"passwordHash\":\"$y$j9T$abc$def\"}", &p) != 0);
    T(parse_plan("{\"disk\":\"sda\",\"sshKey\":\"ssh-ed25519 AAAA\"}", &p) != 0);
    T(parse_plan("{\"disk\":\"sda\",\"autologin\":true}", &p) != 0);
    T(parse_plan("{\"disk\":\"sda\",\"autologin\":false}", &p) != 0);
    install_set_unattended(1);
    T(parse_plan("{\"disk\":\"sda\",\"autologin\":true}", &p) == 0 && p.autologin == 1);
    T(parse_plan("{\"disk\":\"sda\",\"autologin\":false}", &p) == 0 && p.autologin == 0);
    install_set_unattended(0);
    /* hashes: yescrypt or SHA-512 that crypt accepts; nothing else */
    T(install_hash_ok(hash));
    T(!install_hash_ok("plain-password-not-a-hash"));
    T(!install_hash_ok("$1$md5salt$abcdefghijklmnopqrstuv"));
    T(!install_hash_ok("$6$salt$with:colon:inside:xxxxxxxxxxxxxxxxxxx"));
    T(!install_hash_ok("$6$salt$with\nnewline-xxxxxxxxxxxxxxxxxxxx"));
    T(!install_hash_ok("!$6$locked$xxxxxxxxxxxxxxxxxxxxxxxx"));
    T(!install_hash_ok(""));
    /* SSH keys: one plain public key line */
    T(install_ssh_key_ok("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGq7yT1pM4xK0bZ9w2vQe8Lh3sN5uR6tY7iO8pA9sD0f maintainer@lordos"));
    T(install_ssh_key_ok("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGq7yT1pM4xK0bZ9w2vQe8Lh3sN5uR6tY7iO8pA9sD0f"));
    T(!install_ssh_key_ok("command=\"rm -rf /\" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGq7yT1pM4xK0bZ9w2vQe8Lh3sN5"));
    T(!install_ssh_key_ok("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGq7yT1pM4xK0bZ9w2vQe8Lh3sN5uR6t\nssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQ"));
    T(!install_ssh_key_ok("ssh-dss AAAAB3NzaC1kc3MAAACBAP1/U4EddRIpUt9KnC7s5Of2EbdSPO9EAMMeP4C2USZpRV1AIlH7WT2NWPq/xfW6M"));
    T(!install_ssh_key_ok("ssh-ed25519 short"));

    /* mode manual: its fields reach manual.c as they were sent, and malformed ones refuse the plan */
    static MpPlan m;
    T(parse_plan("{\"mode\":\"manual\",\"disk\":\"sda\",\"tableTag\":\"00112233445566778899aabbccddeeff-deadbeef\",\"delete\":\"3\","
                 "\"create\":\"root:41943040:83886079\",\"homePart\":2,\"homeFormat\":false,\"espPart\":1,\"espFormat\":true,\"swapPart\":5,\"swap\":0}", &p) == 0);
    T(fsp_mode(p.mode) == 2 && !strcmp(p.table_tag, "00112233445566778899aabbccddeeff-deadbeef") && !strcmp(p.del, "3"));
    T(p.home_part == 2 && !p.home_format && p.esp_part == 1 && p.esp_format && p.swap_part == 5 && p.root_part == 0);
    T(manual_plan(&p, &m) == 0 && m.ndel == 1 && m.del[0] == 3 && m.nadd == 1 && m.add[0].role == MP_ROOT && m.part[MP_HOME] == 2 && m.esp_format);
    T(parse_plan("{\"mode\":\"manual\",\"rootPart\":-1}", &p) == 0 && manual_plan(&p, &m) != 0);
    T(parse_plan("{\"mode\":\"manual\",\"rootPart\":129}", &p) == 0 && manual_plan(&p, &m) != 0);
    T(parse_plan("{\"mode\":\"manual\",\"delete\":\"3,3\"}", &p) == 0 && manual_plan(&p, &m) != 0);
    T(parse_plan("{\"mode\":\"manual\",\"create\":\"root:1:2,root:3:4\"}", &p) == 0 && manual_plan(&p, &m) != 0);
    T(parse_plan("{\"mode\":\"manual\",\"create\":\"boot:1:2\"}", &p) == 0 && manual_plan(&p, &m) != 0);
    /* a list too long for its field refuses the plan instead of being cut */
    { static char big[1300]; strcpy(big, "{\"mode\":\"manual\",\"delete\":\""); size_t l = strlen(big);
      while (l < sizeof big - 8) { memcpy(big + l, "1,", 2); l += 2; } strcpy(big + l, "1\"}");
      T(parse_plan(big, &p) != 0); }
    /* it is refused like any other plan when the disk isn't listed */
    T(parse_plan("{\"mode\":\"manual\",\"disk\":\"zzz9\",\"rootPart\":4,\"espPart\":1}", &p) == 0 && !strcmp(validate(&p), "install.diskNotListed"));
    printf("install-plan-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
