/* sqlc_test.c — lord-server's side of Databases (src/sql/sqlc.c) without a server: the favorites.json validator
 * (no key that could hold a secret, anywhere; ports, hosts, ids, sizes, types), the lookup with its defaults, the
 * Unix socket rule, and saving / reading the file in a throwaway home (0600, a refused file leaves the old one).
 *
 *   make -C server build/sqlc-test        (also in `make test`) */
#include "../src/sql/sqlc.h"

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

int main(void) {
    unsetenv("LORDOS_SESSIOND");
    test_validator();
    test_find();
    test_socket();
    test_files();
    printf("sqlc-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
