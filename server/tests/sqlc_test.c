/* sqlc_test.c — lord-server's side of Databases (src/sql/sqlc.c) without a server: the favorites.json validator
 * (no key that could hold a secret, anywhere; ports, hosts, ids, sizes, types), the lookup with its defaults, the
 * Unix socket rule, and saving / reading the file in a throwaway home (0600, a refused file leaves the old one); and the
 * SSH tunnel's checks (sshtun.c): the kept server key, its fingerprint, the known_hosts line, ssh's argv, its errors.
 *
 *   make -C server build/sqlc-test        (also in `make test`) */
#include "../src/sql/sqlc.h"
#include "../src/sql/sshtun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int valid(const char *j) { return sql_favorites_valid(j, strlen(j), NULL, 0); }

/* a file with one favorite whose body is fav */
static int valid_fav(const char *fav) {
    char j[8192];
    snprintf(j, sizeof j, "{\"version\":1,\"groups\":[],\"favorites\":[{\"id\":\"f1\",%s}]}", fav);
    return valid(j);
}

static const char SAMPLE[] =
    "{ \"version\": 1,\n"
    "  \"groups\": [{ \"id\": \"g1\", \"name\": \"Clients\", \"parent\": null, \"expanded\": true }],\n"
    "  \"favorites\": [{\n"
    "    \"id\": \"prod-shop\", \"name\": \"prod-shop\", \"group\": \"g1\", \"color\": 0,\n"
    "    \"type\": \"tcp\", \"host\": \"db.example.net\", \"port\": 3306, \"user\": \"shop_admin\", \"database\": \"shop\",\n"
    "    \"socket\": \"\", \"ssh\": { \"host\": \"\", \"port\": 22, \"user\": \"\", \"key\": \"\" },\n"
    "    \"tls\": { \"mode\": \"preferred\", \"ca\": \"\", \"cert\": \"\", \"key\": \"\" },\n"
    "    \"cloud\": { \"service\": \"\", \"region\": \"\", \"iamProfile\": \"\" },\n"
    "    \"readOnly\": false, \"production\": true, \"timeZone\": \"\", \"trust\": \"\",\n"
    "    \"imported\": { \"from\": \"sequel-ace\", \"id\": \"\xe2\x80\xa6\" }, \"quick\": false }] }";

static void test_validator(void) {
    CHECK(valid(SAMPLE), "the sample of docs/DATABASES.md");
    CHECK(valid("{\"version\":1,\"groups\":[],\"favorites\":[]}"), "empty file");
    CHECK(valid("{\"version\":1}"), "no lists");
    /* not an object, not JSON, wrong version */
    CHECK(!valid("[]"), "array");
    CHECK(!valid("\"x\""), "string");
    CHECK(!valid("{\"version\":1,"), "not JSON");
    CHECK(!valid("{\"groups\":[]}"), "no version");
    CHECK(!valid("{\"version\":2}"), "version 2");
    CHECK(!valid("{\"version\":1,\"favorites\":{}}"), "favorites not a list");
    CHECK(!valid("{\"version\":1,\"favorites\":[1]}"), "favorite not an object");
    CHECK(!valid("{\"version\":1,\"groups\":[\"g\"]}"), "group not an object");
    CHECK(valid("{\"version\":1,\"groups\":[{\"id\":\"g\",\"name\":\"A\",\"color\":4}]}"), "a group with a color");
    CHECK(valid("{\"version\":1,\"groups\":[{\"id\":\"g\",\"color\":null}]}"), "a group without one");
    CHECK(!valid("{\"version\":1,\"groups\":[{\"id\":\"g\",\"color\":7}]}"), "a group color past gray");
    CHECK(!valid("{\"version\":1,\"groups\":[{\"id\":\"g\",\"color\":1.5}]}"), "a group color not whole");
    CHECK(!valid("{\"version\":1,\"groups\":[{\"id\":\"g\",\"color\":\"red\"}]}"), "a group color as text");
    /* secrets, wherever they are */
    CHECK(!valid_fav("\"password\":\"x\""), "password");
    CHECK(!valid_fav("\"Password\":\"\""), "Password, even empty");
    CHECK(!valid_fav("\"pass\":\"x\""), "pass");
    CHECK(!valid_fav("\"pwd\":\"x\""), "pwd");
    CHECK(!valid_fav("\"sshPassword\":\"x\""), "sshPassword");
    CHECK(!valid_fav("\"ssh\":{\"host\":\"h\",\"passphrase\":\"x\"}"), "ssh.passphrase");
    CHECK(!valid_fav("\"tls\":{\"mode\":\"off\",\"keySecret\":\"x\"}"), "tls.keySecret");
    CHECK(!valid_fav("\"cloud\":{\"token\":\"x\"}"), "cloud.token");
    CHECK(!valid_fav("\"extra\":[{\"api_key\":\"x\"}]"), "nested in an array");
    CHECK(!valid("{\"version\":1,\"secret\":\"x\"}"), "top-level secret");
    CHECK(!valid("{\"version\":1,\"groups\":[{\"id\":\"g\",\"password\":\"x\"}]}"), "in a group");
    CHECK(valid_fav("\"ssh\":{\"key\":\"/home/u/.ssh/id_ed25519\"},\"tls\":{\"key\":\"certs/client.key\"}"), "key paths are fine");
    /* ports */
    CHECK(valid_fav("\"port\":1") && valid_fav("\"port\":65535"), "port range");
    CHECK(!valid_fav("\"port\":0"), "port 0");
    CHECK(!valid_fav("\"port\":65536"), "port 65536");
    CHECK(!valid_fav("\"port\":-3306"), "negative port");
    CHECK(!valid_fav("\"port\":3306.5"), "fractional port");
    CHECK(!valid_fav("\"port\":\"3306\""), "port as text");
    CHECK(!valid_fav("\"ssh\":{\"port\":70000}"), "ssh port");
    /* hosts */
    CHECK(valid_fav("\"host\":\"10.0.0.5\"") && valid_fav("\"host\":\"::1\"") && valid_fav("\"host\":\"db-1.example.net\""), "good hosts");
    CHECK(!valid_fav("\"host\":\"db example\""), "host with a space");
    CHECK(!valid_fav("\"host\":\"db\\u0001x\""), "host with a control character");
    CHECK(!valid_fav("\"host\":\"db\\nx\""), "host with a newline");
    CHECK(!valid_fav("\"host\":\"-oProxyCommand=x\""), "host starting with -");
    CHECK(!valid_fav("\"host\":\"a/b\""), "host with a slash");
    CHECK(!valid_fav("\"ssh\":{\"host\":\"a b\"}"), "ssh host with a space");
    char longhost[400];
    memset(longhost, 'a', 300);
    longhost[300] = 0;
    char fav[512];
    snprintf(fav, sizeof fav, "\"host\":\"%s\"", longhost);
    CHECK(!valid_fav(fav), "host of 300 characters");
    /* other fields */
    CHECK(!valid_fav("\"type\":\"telnet\""), "bad type");
    CHECK(!valid_fav("\"tls\":{\"mode\":\"maybe\"}"), "bad tls mode");
    CHECK(!valid_fav("\"color\":7") && !valid_fav("\"color\":\"red\""), "bad color");
    CHECK(valid_fav("\"color\":null") && valid_fav("\"color\":6"), "good color");
    CHECK(!valid_fav("\"socket\":\"mysql.sock\""), "relative socket");
    CHECK(!valid_fav("\"user\":\"a\\tb\""), "user with a tab");
    CHECK(!valid_fav("\"trust\":\"AB:CD:ZZ\""), "trust not hex");
    CHECK(valid_fav("\"trust\":\"AB:cd:01\""), "trust hex");
    CHECK(!valid_fav("\"readOnly\":1"), "flag not a boolean");
    CHECK(!valid("{\"version\":1,\"favorites\":[{\"id\":\"a b\"}]}"), "id with a space");
    CHECK(!valid("{\"version\":1,\"favorites\":[{\"name\":\"no id\"}]}"), "no id");
    CHECK(!valid("{\"version\":1,\"favorites\":[{\"id\":\"a\"},{\"id\":\"a\"}]}"), "duplicate ids");
    /* sizes: 500 favorites fit, 501 don't */
    size_t cap = 600 * 32 + 128;
    char *big = malloc(cap);
    for (int count = 500; count <= 501; count++) {
        size_t o = (size_t)snprintf(big, cap, "{\"version\":1,\"favorites\":[");
        for (int i = 0; i < count; i++) o += (size_t)snprintf(big + o, cap - o, "%s{\"id\":\"f%d\"}", i ? "," : "", i);
        snprintf(big + o, cap - o, "]}");
        CHECK(valid(big) == (count == 500), "%d favorites", count);
    }
    free(big);
    /* a file over the size limit */
    size_t hn = SQL_FAV_FILE_MAX + 100;
    char *huge = malloc(hn + 1);
    memset(huge, ' ', hn);
    memcpy(huge, "{\"version\":1}", 13);
    huge[hn] = 0;
    CHECK(!sql_favorites_valid(huge, hn, NULL, 0), "file over 1 MB");
    free(huge);
    char why[96] = "";
    const char *sec = "{\"version\":1,\"favorites\":[{\"id\":\"a\",\"password\":\"x\"}]}";
    CHECK(!sql_favorites_valid(sec, strlen(sec), why, sizeof why) && !strcmp(why, "holds a secret"),
          "why: %s", why);
}

static void test_find(void) {
    SqlFav f;
    CHECK(!sql_fav_find(SAMPLE, strlen(SAMPLE), "prod-shop", &f) && !strcmp(f.host, "db.example.net") && f.port == 3306 &&
          !strcmp(f.user, "shop_admin") && !strcmp(f.database, "shop") && !strcmp(f.tls, "preferred") && !strcmp(f.type, "tcp"), "find");
    CHECK(sql_fav_find(SAMPLE, strlen(SAMPLE), "nope", &f), "not found");
    const char *j = "{\"version\":1,\"favorites\":[{\"id\":\"a\",\"host\":\"h\"},{\"id\":\"b\",\"type\":\"socket\",\"socket\":\"/run/mysqld/mysqld.sock\","
                    "\"port\":3307,\"tls\":{\"ca\":\"ca.pem\"},\"trust\":\"AA:BB\",\"cloud\":{\"iamProfile\":\"dev\"}}]}";
    CHECK(!sql_fav_find(j, strlen(j), "a", &f) && f.port == 3306 && !strcmp(f.type, "tcp") && !strcmp(f.tls, "preferred") && !f.ca[0], "defaults");
    CHECK(!sql_fav_find(j, strlen(j), "b", &f) && f.port == 3307 && !strcmp(f.type, "socket") && !strcmp(f.socket, "/run/mysqld/mysqld.sock") &&
          !strcmp(f.ca, "ca.pem") && !strcmp(f.trust, "AA:BB") && !strcmp(f.iam, "dev"), "socket favorite");
    const char *bad = "{\"version\":1,\"favorites\":[{\"id\":\"a\",\"host\":\"h\",\"password\":\"x\"}]}";
    CHECK(sql_fav_find(bad, strlen(bad), "a", &f), "a refused file finds nothing");
}

static void test_socket(void) {
    CHECK(sql_socket_ok("/run/mysqld/mysqld.sock", "/run/user/1000"), "mysqld socket");
    CHECK(sql_socket_ok("/tmp/mysql.sock", "/run/user/1000"), "/tmp/mysql.sock");
    CHECK(sql_socket_ok("/home/u/.local/mysql.sock", NULL), "socket in home");
    CHECK(!sql_socket_ok("/run/user/1000/bus", NULL), "session bus");
    CHECK(!sql_socket_ok("/run/dbus/system_bus_socket", NULL), "system bus");
    CHECK(!sql_socket_ok("/run/systemd/private", NULL), "systemd");
    CHECK(!sql_socket_ok("/run/lordos/cgroup.sock", NULL), "LordOS's own");
    CHECK(!sql_socket_ok("/tmp/.X11-unix/X0", NULL), "X11");
    CHECK(!sql_socket_ok("/srv/rt/wayland-0", "/srv/rt/"), "runtime dir");
    CHECK(sql_socket_ok("/run/userdata/mysql.sock", NULL), "a prefix is not a folder");
    CHECK(!sql_socket_ok("relative.sock", NULL), "relative");
}

static void test_files(void) {
    char dir[] = "/tmp/sqlc-test.XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    setenv("HOME", dir, 1);
    size_t n;
    char *s = sql_favorites_read(&n);
    CHECK(s && !strcmp(s, "{\"version\":1,\"groups\":[],\"favorites\":[]}"), "missing file reads as the empty one");
    free(s);
    CHECK(sql_favorites_save(SAMPLE, strlen(SAMPLE)) == 0, "save");
    struct stat st;
    CHECK(!stat(sql_favorites_path(), &st) && (st.st_mode & 0777) == 0600, "0600");
    char d2[600];
    snprintf(d2, sizeof d2, "%s/.config/lordos/databases", dir);
    CHECK(!stat(d2, &st) && (st.st_mode & 0777) == 0700, "folder 0700");
    s = sql_favorites_read(&n);
    CHECK(s && n == strlen(SAMPLE) && !memcmp(s, SAMPLE, n), "read back");
    free(s);
    const char *bad = "{\"version\":1,\"favorites\":[{\"id\":\"a\",\"password\":\"hunter2\"}]}";
    CHECK(sql_favorites_save(bad, strlen(bad)) == 1, "refused");
    s = sql_favorites_read(&n);
    CHECK(s && n == strlen(SAMPLE), "a refused save leaves the file as it was");
    free(s);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* nothing */ }
}

/* ---- the SSH tunnel (sshtun.c) ---- */
#define ED  "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIELR56/bq7yBXo5+IGCiVTclE52yTok7jaoB8rrUefpd"
#define ED_SHA "SHA256:XURXAEGf6vKKSZN36UF7OivoDBFwV9TG9bFUxcHHENw"

static int has_arg(char **argv, const char *a) { for (int i = 0; argv[i]; i++) if (!strcmp(argv[i], a)) return 1; return 0; }
static int index_of(char **argv, const char *a) { for (int i = 0; argv[i]; i++) if (!strcmp(argv[i], a)) return i; return -1; }

static void test_ssh(void) {
    char out[256], buf[6144], *argv[SSH_ARGV_MAX];
    /* the kept key */
    CHECK(ssh_hostkey_ok(ED), "an ed25519 key");
    CHECK(!ssh_hostkey_ok("ssh-ed25519"), "no base64");
    CHECK(!ssh_hostkey_ok("ssh-dss " "AAAAC3NzaC1lZDI1NTE5AAAAIELR56/bq7yBXo5+IGCiVTclE52yTok7jaoB8rrUefpd"), "a type ssh won't take");
    CHECK(!ssh_hostkey_ok("ssh-rsa AAAAC3NzaC1lZDI1NTE5AAAAIELR56/bq7yBXo5+IGCiVTclE52yTok7jaoB8rrUefpd"), "a blob of another type");
    CHECK(!ssh_hostkey_ok(ED " extra"), "a third field");
    CHECK(!ssh_hostkey_ok("ssh-ed25519 AAAA*not*base64"), "not base64");
    CHECK(!ssh_hostkey_ok("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIELR56/bq7yBXo5+IGCiVTclE52yTok7jaoB8rrUefp"), "base64 cut short");
    /* the fingerprint, as ssh-keygen -l prints it */
    CHECK(!ssh_fingerprint(ED, out, sizeof out) && !strcmp(out, ED_SHA), "fingerprint %s", out);
    CHECK(ssh_fingerprint("ssh-ed25519 x", out, sizeof out) == -1, "no fingerprint for a bad key");
    /* known_hosts lines */
    CHECK(!ssh_known_line("54.232.3.17", 22, ED, out, sizeof out) && !strcmp(out, "54.232.3.17 " ED "\n"), "port 22");
    CHECK(!ssh_known_line("db.example", 2222, ED, out, sizeof out) && !strcmp(out, "[db.example]:2222 " ED "\n"), "another port");
    CHECK(ssh_known_line("-oProxyCommand=x", 22, ED, out, sizeof out) == -1, "a host that is an option");
    CHECK(ssh_known_line("a b", 22, ED, out, sizeof out) == -1, "a host with a space");
    CHECK(!ssh_key_of_line("[db.example]:2222 " ED "\n", out, sizeof out) && !strcmp(out, ED), "the key back from ssh's line");
    CHECK(ssh_key_of_line("", out, sizeof out) == -1, "an empty known_hosts");
    /* argv */
    SshTun t = { "54.232.3.17", "root", 22, "localhost", 3306, "/home/u/.ssh/id_ed25519", "/run/user/1000/lordos-sql/known-1", 0 };
    int n = ssh_argv(&t, buf, sizeof buf, argv, SSH_ARGV_MAX);
    CHECK(n > 0 && !strcmp(argv[0], "ssh") && argv[n] == NULL, "argv built");
    CHECK(n > 0 && has_arg(argv, "-F") && !strcmp(argv[index_of(argv, "-F") + 1], "none"), "never the user's config");
    CHECK(n > 0 && has_arg(argv, "StrictHostKeyChecking=yes") && has_arg(argv, "BatchMode=yes") && has_arg(argv, "ProxyCommand=none"), "strict, batch, no proxy command");
    CHECK(n > 0 && has_arg(argv, "-W") && !strcmp(argv[index_of(argv, "-W") + 1], "localhost:3306"), "-W host:port");
    CHECK(n > 0 && !strcmp(argv[n - 2], "--") && !strcmp(argv[n - 1], "54.232.3.17"), "-- then the server, last");
    CHECK(n > 0 && has_arg(argv, "IdentityFile=/home/u/.ssh/id_ed25519") && has_arg(argv, "IdentitiesOnly=yes"), "only that key");
    SshTun v6 = t; v6.db_host = "::1";
    n = ssh_argv(&v6, buf, sizeof buf, argv, SSH_ARGV_MAX);
    CHECK(n > 0 && !strcmp(argv[index_of(argv, "-W") + 1], "[::1]:3306"), "an IPv6 database host");
    SshTun pr = t; pr.probe = 1; pr.key_path = NULL;
    n = ssh_argv(&pr, buf, sizeof buf, argv, SSH_ARGV_MAX);
    CHECK(n > 0 && has_arg(argv, "StrictHostKeyChecking=accept-new") && has_arg(argv, "PubkeyAuthentication=no") && has_arg(argv, "PasswordAuthentication=no") &&
          has_arg(argv, "-N") && !has_arg(argv, "-W") && index_of(argv, "IdentityFile=/home/u/.ssh/id_ed25519") < 0, "the probe authenticates nothing and forwards nothing");
    SshTun b1 = t; b1.ssh_host = "-oProxyCommand=sh";
    CHECK(ssh_argv(&b1, buf, sizeof buf, argv, SSH_ARGV_MAX) == -1, "a server that is an option");
    SshTun b2 = t; b2.ssh_user = "-oLocalCommand=x";
    CHECK(ssh_argv(&b2, buf, sizeof buf, argv, SSH_ARGV_MAX) == -1, "a user that is an option");
    SshTun b3 = t; b3.key_path = "/home/u/my key";
    CHECK(ssh_argv(&b3, buf, sizeof buf, argv, SSH_ARGV_MAX) == -1, "a key path with a space");
    SshTun b4 = t; b4.key_path = "/home/u/%h";
    CHECK(ssh_argv(&b4, buf, sizeof buf, argv, SSH_ARGV_MAX) == -1, "%s", "a key path with a %-token");
    SshTun b5 = t; b5.db_host = "db;rm";
    CHECK(ssh_argv(&b5, buf, sizeof buf, argv, SSH_ARGV_MAX) == -1, "a database host with a ;");
    SshTun b6 = t; b6.ssh_port = 70000;
    CHECK(ssh_argv(&b6, buf, sizeof buf, argv, SSH_ARGV_MAX) == -1, "a port out of range");
    CHECK(ssh_argv(&t, buf, 64, argv, SSH_ARGV_MAX) == -1, "a buffer too small");
    /* ssh's errors */
    CHECK(!strcmp(ssh_err_code("root@1.2.3.4: Permission denied (publickey)."), "sshauth"), "auth");
    CHECK(!strcmp(ssh_err_code("@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@"), "hostkey"), "changed key");
    CHECK(!strcmp(ssh_err_code("Host key verification failed."), "hostkey"), "verification");
    CHECK(!strcmp(ssh_err_code("ssh: Could not resolve hostname x: Name or service not known"), "dns"), "dns");
    CHECK(!strcmp(ssh_err_code("ssh: connect to host 1.2.3.4 port 22: Connection refused"), "connect"), "refused");
    CHECK(!strcmp(ssh_err_code("channel 0: open failed: connect failed: Connection refused\nstdio forwarding failed"), "sshforward"), "forward");
    CHECK(!strcmp(ssh_err_code("Load key \"/x\": bad permissions"), "sshkey"), "key permissions");
    CHECK(!strcmp(ssh_err_code("something else"), "ssh"), "anything else");
    /* the favorite keeps a valid key or none */
    CHECK(valid_fav("\"type\":\"ssh\",\"host\":\"localhost\",\"ssh\":{\"host\":\"1.2.3.4\",\"user\":\"root\",\"key\":\"~/.ssh/k\",\"hostKey\":\"" ED "\"}"), "a favorite with its server key");
    CHECK(!valid_fav("\"type\":\"ssh\",\"host\":\"localhost\",\"ssh\":{\"host\":\"1.2.3.4\",\"hostKey\":\"ssh-ed25519 junk\"}"), "a favorite with a bad server key");
    CHECK(valid_fav("\"type\":\"ssh\",\"host\":\"localhost\",\"ssh\":{\"host\":\"1.2.3.4\",\"hostKey\":\"\"}"), "a favorite whose key is not known yet");
}

int main(void) {
    unsetenv("LORDOS_SESSIOND");
    test_validator();
    test_find();
    test_socket();
    test_files();
    test_ssh();
    printf("sqlc-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
