/* autoinstall_test.c — lordos-auto.conf's reader (src/install/autoinstall.c): defaults, refusals and the
 * checks that decide whether a disk gets erased. Nothing here touches a disk. */
#define AUTOINSTALL_TEST
#include "install/autoinstall.c"

static int fails, checks;
#define T(c) do { checks++; if (!(c)) { fails++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

static const char *parse(const char *text, Conf *c) {
    static char buf[CONF_MAX];
    int ln;
    snprintf(buf, sizeof buf, "%s", text);
    return parse_conf(buf, c, &ln);
}

int main(void) {
    static Conf c;
    /* the defaults */
    T(parse("# only comments\n\n", &c) == NULL);
    T(!strcmp(c.mode, "erase") && !strcmp(c.lang, "en") && !strcmp(c.locale, "en_US.UTF-8") && !strcmp(c.timezone, "UTC"));
    T(!strcmp(c.keyboard, "us") && !strcmp(c.autologin, "no") && !strcmp(c.after, "reboot") && !c.has_disk_bytes && !c.erase[0]);
    /* a full file, spaces around '=' and values with spaces */
    T(parse("disk = vda\ndisk_bytes = 34359738368\nmode = erase\nerase = yes\nuser = alex\nfullname = Alex Rivera\n"
            "password_hash = $6$abc$def\nhostname = lordos-test\nswap = 2048\nautologin = yes\nafter = poweroff\n", &c) == NULL);
    T(!strcmp(c.disk, "vda") && c.has_disk_bytes && c.disk_bytes == 34359738368ull && !strcmp(c.fullname, "Alex Rivera"));
    T(c.swap == 2048 && !strcmp(c.after, "poweroff") && !strcmp(c.password_hash, "$6$abc$def"));
    /* refusals: an unknown key (a typo must never fall back to a default), a line without '=', bad numbers */
    T(parse("dsik = vda\n", &c) != NULL);
    T(parse("erase yes\n", &c) != NULL);
    T(parse("disk_bytes = -1\n", &c) != NULL);
    T(parse("disk_bytes = 12GB\n", &c) != NULL);
    T(parse("disk_bytes = 99999999999999999999999\n", &c) != NULL);
    T(parse("swap = 20000\n", &c) != NULL);
    T(parse("hostname = 0123456789012345678901234567890123456789012345678901234567890123456789\n", &c) != NULL);
    /* the target: no disk, no exact size, a disk that doesn't exist → stop before anything */
    unsigned long long fs, fe;
    T(parse("user = alex\n", &c) == NULL && check(&c, NULL, &fs, &fe) == 1);
    T(parse("disk = vda\n", &c) == NULL && check(&c, NULL, &fs, &fe) == 1);
    T(parse("disk = ../sda\ndisk_bytes = 1\n", &c) == NULL && check(&c, NULL, &fs, &fe) == 1);
    T(parse("disk = nosuchdisk0\ndisk_bytes = 1\n", &c) == NULL && check(&c, NULL, &fs, &fe) == 1);
    /* the whole-disk name of a device */
    char par[64];
    parent_disk("/dev/nosuchdisk9", par, sizeof par);
    T(!strcmp(par, "nosuchdisk9"));
    printf("autoinstall-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
