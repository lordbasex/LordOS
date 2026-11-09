/* cloud_test.c — Cloud without a cloud (docs/CLOUD.md 16): the closed rc method table and the request built for it,
 * the HTTP answers, names and paths under ~/Cloud, the provider and website tables, `rclone authorize`'s output,
 * the VFS cache metadata, the status merge, the eviction choice; then, in a throwaway home with no rclone, the
 * accounts, keep and links files, and Free Up Space on a real cache tree (open, changed, waiting and kept files
 * stay). Built with ASan and UBSan, and with CLOUD_TEST (the rclone binary can be pointed elsewhere). */
#include "../src/cloud/cloud_pure.h"
#include "../src/cloud/cloud_int.h"
#include "../src/sec/json.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static void t_methods(void) {
    /* every method the cloud code may call, and nothing else */
    static const char *const allowed[] = { "core/version", "config/providers", "config/create", "config/update", "config/delete",
        "config/get", "mount/mount", "mount/unmount", "mount/listmounts", "operations/about", "operations/publiclink",
        "operations/fsinfo", "core/stats", "core/transferred", "core/bwlimit", "vfs/stats", "vfs/queue", "vfs/queue-set-expiry",
        "vfs/refresh", "vfs/forget", "core/quit", "operations/cleanup", "operations/size", "job/status", "job/stop", "operations/list", "operations/stat", "operations/copyfile", "fscache/clear", NULL };
    static const char *const never[] = { "core/command", "config/dump", "config/show", "config/password", "config/setpath",
        "config/unlock", "options/set", "options/get", "rc/noop", "rc/list", "rc/error", "operations/delete",
        "operations/deletefile", "operations/purge", "operations/copyurl", "operations/uploadfile", "operations/movefile",
        "operations/rmdirs", "operations/mkdir", "backend/command", "serve/start", "job/batch", "sync/sync", "sync/copy",
        "sync/move", "pluginsctl/addPlugin", "debug/set-gc-percent", "core/obscure", "mount/unmountall", "job/list", "job/stopgroup", "operations/check", NULL };
    int n = 0;
    for (int m = 0; m < RCM_COUNT; m++) {
        const char *p = rc_method_path(m);
        CHECK(p != NULL);
        if (!p) continue;
        int ok = 0;
        for (int i = 0; allowed[i]; i++) ok |= !strcmp(allowed[i], p);
        CHECK(ok);
        for (int i = 0; never[i]; i++) CHECK(strcmp(never[i], p));
        for (int k = 0; k < m; k++) CHECK(strcmp(rc_method_path(k), p));
        n++;
    }
    CHECK(n == RCM_COUNT);
    CHECK(rc_method_path(-1) == NULL && rc_method_path(RCM_COUNT) == NULL && rc_method_path(1000) == NULL);
    char h[512];
    CHECK(rc_build_header(h, sizeof h, RCM_COUNT, "dTpw", 2) == -1);
    CHECK(rc_build_header(h, sizeof h, -3, "dTpw", 2) == -1);
    CHECK(rc_build_header(h, sizeof h, RCM_ABOUT, "dTpw\r\nX: y", 2) == -1);     /* nothing can be injected in the header */
    CHECK(rc_build_header(h, sizeof h, RCM_ABOUT, "", 2) == -1);
    int l = rc_build_header(h, sizeof h, RCM_ABOUT, "dTpw", 13);
    CHECK(l > 0 && !strncmp(h, "POST /operations/about HTTP/1.0\r\n", 33) && strstr(h, "Authorization: Basic dTpw\r\n") && strstr(h, "Content-Length: 13\r\n\r\n"));
    CHECK(rc_build_header(h, 20, RCM_ABOUT, "dTpw", 13) == -1);
    char b[64];
    cloud_b64((const unsigned char *)"u:p", 3, b);
    CHECK(!strcmp(b, "dTpw"));
    cloud_b64((const unsigned char *)"ab", 2, b);
    CHECK(!strcmp(b, "YWI="));
}

static void t_http(void) {
    int st;
    char *body;
    size_t bl;
    char a[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}trailing";
    CHECK(!rc_parse_response(a, strlen(a), &st, &body, &bl) && st == 200 && bl == 7 && !strcmp(body, "{\"a\":1}"));
    char c[] = "HTTP/1.1 500 Internal\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{\"er\r\n6\r\nror\":1\r\n1\r\n}\r\n0\r\n\r\n";
    CHECK(!rc_parse_response(c, strlen(c), &st, &body, &bl) && st == 500 && !strcmp(body, "{\"error\":1}"));
    char d[] = "HTTP/1.0 200 OK\r\nServer: rclone\r\n\r\n{}";
    CHECK(!rc_parse_response(d, strlen(d), &st, &body, &bl) && bl == 2);
    char cut[] = "HTTP/1.1 200 OK\r\nContent-Length: 70\r\n\r\n{}";
    CHECK(rc_parse_response(cut, strlen(cut), &st, &body, &bl) == -1);
    char junk[] = "SSH-2.0-OpenSSH\r\n\r\n";
    CHECK(rc_parse_response(junk, strlen(junk), &st, &body, &bl) == -1);
    char bad[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n";
    CHECK(rc_parse_response(bad, strlen(bad), &st, &body, &bl) == -1);
    char nohdr[] = "HTTP/1.1 200 OK\r\nContent-Length: 2";
    CHECK(rc_parse_response(nohdr, strlen(nohdr), &st, &body, &bl) == -1);
}

static void t_paths(void) {
    CHECK(cloud_name_ok("Work") && cloud_name_ok("Personal drive") && cloud_name_ok("Trabajo — señora") && cloud_name_ok("日本"));
    CHECK(!cloud_name_ok("") && !cloud_name_ok(NULL) && !cloud_name_ok(".hidden") && !cloud_name_ok("a/b") && !cloud_name_ok(" lead") && !cloud_name_ok("trail "));
    CHECK(!cloud_name_ok("a\nb") && !cloud_name_ok("a\x7f") && !cloud_name_ok("\xc2\x85x") && !cloud_name_ok("\xff") && !cloud_name_ok("\xc0\xaf"));
    CHECK(!cloud_name_ok("..") && !cloud_name_ok("."));
    char longn[80];
    memset(longn, 'a', 65);
    longn[65] = 0;
    CHECK(!cloud_name_ok(longn));
    CHECK(cloud_rel_ok("") && cloud_rel_ok("a") && cloud_rel_ok("a/b/c.txt") && cloud_rel_ok("..a/b..") && cloud_rel_ok("a/.b"));
    CHECK(!cloud_rel_ok("..") && !cloud_rel_ok("a/..") && !cloud_rel_ok("../a") && !cloud_rel_ok("a/../b") && !cloud_rel_ok("./a") && !cloud_rel_ok("a/./b"));
    CHECK(!cloud_rel_ok("/a") && !cloud_rel_ok("a/") && !cloud_rel_ok("a//b") && !cloud_rel_ok("a\tb") && !cloud_rel_ok(NULL));
    char n[80], r[1100];
    const char *root = "/home/u/Cloud";
    CHECK(!cloud_split(root, "/home/u/Cloud/Work", n, sizeof n, r, sizeof r) && !strcmp(n, "Work") && !r[0]);
    CHECK(!cloud_split(root, "/home/u/Cloud/Work/Projects/a.txt", n, sizeof n, r, sizeof r) && !strcmp(r, "Projects/a.txt"));
    CHECK(cloud_split(root, "/home/u/Cloud", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "/home/u/Cloudy/Work/a", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "/home/u/Cloud/Work/../../.ssh/id", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "/home/u/Cloud/../Cloud/Work", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "/home/u/Cloud//Work", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "/home/u/Cloud/Work/", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "/home/u/Cloud/Work/a/", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, "Cloud/Work/a", n, sizeof n, r, sizeof r));
    CHECK(cloud_split(root, NULL, n, sizeof n, r, sizeof r));
    CHECK(cloud_key_ok("url") && cloud_key_ok("access_key_id") && !cloud_key_ok("Url") && !cloud_key_ok("a b") && !cloud_key_ok("") && !cloud_key_ok("a=b"));
    CHECK(cloud_value_ok("https://dav.example.test/remote.php") && cloud_value_ok("") && !cloud_value_ok("a\nb=c") && !cloud_value_ok("a\rb") && !cloud_value_ok(NULL));
    const char *kept[] = { "Projects", "Photos/2026" };
    CHECK(cloud_kept(kept, 2, "Projects") && cloud_kept(kept, 2, "Projects/a/b") && cloud_kept(kept, 2, "Photos/2026/x.jpg"));
    CHECK(!cloud_kept(kept, 2, "Projectsx") && !cloud_kept(kept, 2, "Photos") && !cloud_kept(kept, 2, "Photos/2025/x") && !cloud_kept(kept, 0, "a"));
    const char *all[] = { "" };
    CHECK(cloud_kept(all, 1, "anything/at/all"));
    {
        /* #71: which folders are still coming down as a whole */
        const char *k2[] = { "Projects", "Photos/2026" };
        unsigned char busy[2] = { 0, 1 };
        CHECK(cloud_kept_busy(k2, busy, 2, "Photos/2026") && cloud_kept_busy(k2, busy, 2, "Photos/2026/trip") && cloud_kept_busy(k2, busy, 2, "Photos"));
        CHECK(cloud_kept_busy(k2, busy, 2, ""));                                  /* the account's folder holds it */
        CHECK(!cloud_kept_busy(k2, busy, 2, "Projects") && !cloud_kept_busy(k2, busy, 2, "Photos/2025") && !cloud_kept_busy(k2, busy, 2, "Photo"));
        CHECK(!cloud_kept_busy(k2, busy, 2, "Photos/20") && !cloud_kept_busy(k2, NULL, 2, "Photos") && !cloud_kept_busy(k2, busy, 2, NULL));
        unsigned char none[2] = { 0, 0 };
        CHECK(!cloud_kept_busy(k2, none, 2, "Photos/2026") && !cloud_kept_busy(k2, none, 2, ""));
        unsigned char whole = 1;
        CHECK(cloud_kept_busy(all, &whole, 1, "x/y") && cloud_kept_busy(all, &whole, 1, ""));
    }
}

static int has_brand(const char *s) {
    static const char *const bad[] = { "Microsoft", "Windows", "Apple", "iCloud", "OneDrive", "macOS", "Outlook", NULL };
    for (int i = 0; s && bad[i]; i++) if (strstr(s, bad[i])) return 1;
    return 0;
}

static void t_kinds(void) {
    int n = cloud_kind_count();
    CHECK(n >= 20);
    for (int i = 0; i < n; i++) {
        const CloudKind *k = cloud_kind_at(i);
        CHECK(k && k->kind && k->type && k->label && k->group);
        CHECK(!strcmp(k->group, "oauth") || !strcmp(k->group, "server") || !strcmp(k->group, "object") || !strcmp(k->group, "crypt"));
        CHECK(!strstr(k->label, "Office Drive"));                 /* rclone's names now (17.2 item 2) */
        CHECK(!k->variant || (!has_brand(k->label) && strcmp(k->kind, k->type)));
        CHECK(!has_brand(k->hint));
        CHECK(cloud_kind(k->kind) == k);
        CHECK(!k->website || cloud_url_ok(k->website));
        for (int j = 0; j < i; j++) CHECK(strcmp(cloud_kind_at(j)->kind, k->kind));
    }
    CHECK(cloud_kind_at(-1) == NULL && cloud_kind_at(n) == NULL && cloud_kind("core") == NULL && cloud_kind(NULL) == NULL);
    const CloudKind *nc = cloud_kind("nextcloud");
    CHECK(nc && !strcmp(nc->type, "webdav") && cloud_field_hidden(nc, "vendor") && !cloud_field_hidden(nc, "url") && !cloud_field_hidden(nc, "vendo"));
    CHECK(!cloud_field_hidden(cloud_kind("dropbox"), "vendor"));
    /* the website table */
    char u[400];
    CHECK(cloud_site_url(cloud_kind("dropbox"), NULL, u, sizeof u) && !strncmp(u, "https://", 8));
    CHECK(cloud_site_url(cloud_kind("drive"), "ignored", u, sizeof u) && !strcmp(u, "https://drive.google.com/"));
    CHECK(cloud_site_url(cloud_kind("webdav"), "https://dav.example.test/x", u, sizeof u) && !strcmp(u, "https://dav.example.test/x"));
    CHECK(!cloud_site_url(cloud_kind("webdav"), "http://dav.example.test/x", u, sizeof u));      /* https only */
    CHECK(!cloud_site_url(cloud_kind("webdav"), NULL, u, sizeof u));
    CHECK(!cloud_site_url(cloud_kind("s3"), "https://s3.example.test/\"><script>", u, sizeof u));
    CHECK(!cloud_site_url(cloud_kind("s3"), "javascript:alert(1)", u, sizeof u));
    CHECK(!cloud_site_url(cloud_kind("sftp"), "https://x.example.test", u, sizeof u));
    CHECK(!cloud_site_url(NULL, NULL, u, sizeof u));
    CHECK(!cloud_url_ok("https://") && !cloud_url_ok("https:///x") && !cloud_url_ok("https://a b") && !cloud_url_ok("ftp://x.test"));
}

static void t_rules(void) {
    /* 17.2 item 1: what isn't a storage of its own */
    const char *no[] = { "alias", "archive", "cache", "chunker", "combine", "compress", "hasher", "http", "local", "memory", "union", "tardigrade" };
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) CHECK(cloud_excluded(no[i]));
    CHECK(!cloud_excluded("webdav") && !cloud_excluded("crypt") && !cloud_excluded("s3") && !cloud_excluded("sftp") && cloud_excluded("") && cloud_excluded(NULL));
    const char *dropbox[] = { "client_id", "client_secret", "token", "auth_url", "token_url", "chunk_size" };
    const char *linkbox[] = { "token" };
    const char *s3[] = { "provider", "env_auth", "access_key_id", "secret_access_key", "region", "endpoint" };
    const char *webdav[] = { "url", "vendor", "user", "pass", "bearer_token" };
    const char *b2[] = { "account", "key", "application_key" };
    CHECK(!strcmp(cloud_derive_group("dropbox", dropbox, 6), "oauth"));
    CHECK(!strcmp(cloud_derive_group("linkbox", linkbox, 1), "server"));          /* a token alone is no browser sign-in */
    CHECK(!strcmp(cloud_derive_group("s3", s3, 6), "object"));
    CHECK(!strcmp(cloud_derive_group("webdav", webdav, 5), "server"));
    CHECK(!strcmp(cloud_derive_group("b2", b2, 3), "object"));
    CHECK(!strcmp(cloud_derive_group("crypt", webdav, 5), "crypt"));
    CHECK(!strcmp(cloud_derive_group("x", NULL, 0), "server"));
    /* 17.2 item 3: a question's state for JS, and the ones answered here */
    char id[16];
    const char *st;
    CHECK(!cloud_answer_state("c0a1b2c3d:auth_type_done", id, sizeof id, &st) && !strcmp(id, "c0a1b2c3d") && !strcmp(st, "auth_type_done"));
    CHECK(!cloud_answer_state("c0a1b2c3d:*oauth-confirm,choose_type,,", id, sizeof id, &st) && !strcmp(st, "*oauth-confirm,choose_type,,"));
    CHECK(cloud_answer_state("auth_type_done", id, sizeof id, &st) == -1 && cloud_answer_state(":x", id, sizeof id, &st) == -1);
    CHECK(cloud_answer_state("c0a1b2c3d:", id, sizeof id, &st) == -1 && cloud_answer_state("C0A:x", id, sizeof id, &st) == -1);
    CHECK(cloud_answer_state("../x:y", id, sizeof id, &st) == -1 && cloud_answer_state("c1:a\nb", id, sizeof id, &st) == -1 && cloud_answer_state(NULL, id, sizeof id, &st) == -1);
    CHECK(cloud_answer_state("c0123456789abcdefg:x", id, sizeof id, &st) == -1);
    CHECK(cloud_internal_question("config_refresh_token") && cloud_internal_question("config_is_local"));
    CHECK(!cloud_internal_question("config_type") && !cloud_internal_question("config_2fa") && !cloud_internal_question(NULL));
    const CloudKind *nc = cloud_kind("nextcloud"), *wd = cloud_kind("webdav");
    CHECK(nc && nc->variant && wd && !wd->variant && !strcmp(cloud_kind("jottacloud")->group, "server"));
}

static void t_refused(void) {
    /* B5: what JS may never set, whatever the backend */
    const char *bad[] = { "ssh", "key_file", "pubkey_file", "known_hosts_file", "key_use_agent", "ssh_agent", "server_command",
        "md5sum_command", "sha1sum_command", "bearer_token_command", "unix_socket", "service_account_file", "box_config_file",
        "config_file", "client_certificate_path", "shared_credentials_file", "sse_customer_key_file", "service_principal_file",
        "db_path", "chunk_path", "tmp_upload_path", "env_auth", "use_msi", "use_az", "socks_proxy", "http_proxy",
        "config_token", "config_refresh_token", "auth_url", "token_url", "client_credentials", "local_dir", "x_cmd" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(cloud_option_refused(bad[i], "string"));
    const char *good[] = { "host", "user", "port", "pass", "key_pem", "key_file_pass", "pubkey", "url", "vendor", "endpoint",
        "access_key_id", "secret_access_key", "region", "provider", "remote", "password", "password2", "bucket", "account", "key",
        "use_insecure_cipher", "disable_hashcheck", "shell_type", "root_folder_id", "description" };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) CHECK(!cloud_option_refused(good[i], "string"));
    CHECK(cloud_option_refused("ssh", "SpaceSepList") && cloud_option_refused("anything", "SpaceSepList"));
    CHECK(cloud_option_refused("headers", "CommaSepList") == 0 && cloud_option_refused("x", "stringArray") && cloud_option_refused("x", NULL));
    CHECK(!cloud_option_refused("port", "int") && !cloud_option_refused("chunk_size", "SizeSuffix") && !cloud_option_refused("x", "Tristate"));
    CHECK(cloud_option_refused(NULL, "string") && cloud_option_refused("Bad-Name", "string"));
    /* B4: rclone's words about an account that didn't work */
    CHECK(!strcmp(cloud_failure_code("couldn't connect SSH: ssh: handshake failed: ssh: unable to authenticate, attempted methods [none password], no supported methods remain"), "cloud.authFailed"));
    CHECK(!strcmp(cloud_failure_code("couldn't list files: 401 Unauthorized: "), "cloud.authFailed"));
    CHECK(!strcmp(cloud_failure_code("failed to read private key: x509: decryption password incorrect"), "cloud.authFailed"));
    CHECK(!strcmp(cloud_failure_code("AccessDenied: Access Denied status code: 403"), "cloud.authFailed"));
    CHECK(!strcmp(cloud_failure_code("error in ListJSON: couldn't list files: pcloud error: Invalid 'access_token' provided. (2094)"), "cloud.authFailed"));   /* #71 */
    /* the session summary: lengths and names, never a value */
    {
        char sum[200];
        cloud_session_summary("abcdef", "X-APPLE-WEBAUTH-TOKEN=\"v=2:t=SECRET\"; X-APPLE-WEBAUTH-USER=\"u\";bad name=1; ", sum, sizeof sum);
        CHECK(!strcmp(sum, "trust token 6 chars; cookies: X-APPLE-WEBAUTH-TOKEN X-APPLE-WEBAUTH-USER"));
        CHECK(!strstr(sum, "SECRET"));
        cloud_session_summary("", "", sum, sizeof sum);
        CHECK(!strcmp(sum, "trust token 0 chars; cookies: none"));
    }
    /* an iCloud session the provider no longer takes: a new sign-in, not a mount every 30 s */
    CHECK(!strcmp(cloud_failure_code("requestPCS(iclouddrive): HTTP error 500 (500 Internal Server Error) returned body: \"{\\\"success\\\":false,\\\"error\\\":\\\"Missing X-APPLE-WEBAUTH-TOKEN cookie\\\"}\""), "cloud.signInAgain"));
    CHECK(!strcmp(cloud_failure_code("requestPCS(iclouddrive): HTTP error 421 (421 Misdirected Request)"), "cloud.signInAgain"));
    CHECK(!cloud_failure_code("requestPCS(iclouddrive): HTTP error 503 (503 Service Unavailable)") || strcmp(cloud_failure_code("requestPCS(iclouddrive): HTTP error 503 (503 Service Unavailable)"), "cloud.signInAgain"));
    CHECK(!strcmp(cloud_failure_code("sign in failed: incorrect username or password"), "cloud.authFailed"));   /* #65: an app-specific password */
    CHECK(!strcmp(cloud_failure_code("dial tcp 127.0.0.1:1: connect: connection refused"), "cloud.unreachable"));
    CHECK(!strcmp(cloud_failure_code("error in ListJSON: write tcp 127.0.0.1:38242->127.0.0.1:18991: write: broken pipe"), "cloud.unreachable"));
    CHECK(!strcmp(cloud_failure_code("dial tcp: lookup nowhere.example.test: no such host"), "cloud.unreachable"));
    CHECK(!strcmp(cloud_failure_code("dial tcp 10.255.255.1:22: i/o timeout"), "cloud.unreachable"));
    CHECK(!strcmp(cloud_failure_code("parse \"http://127.0.0.1:99999\": invalid port \":99999\" after host"), "cloud.badAddress"));
    CHECK(!strcmp(cloud_failure_code("Get \"ftp://x\": unsupported protocol scheme \"ftp\""), "cloud.badAddress"));
    CHECK(!strcmp(cloud_failure_code("Propfind \"https://127.0.0.1:18443/\": tls: failed to verify certificate: x509: certificate signed by unknown authority"), "cloud.certUntrusted"));
    CHECK(!strcmp(cloud_failure_code("ssh: handshake failed: knownhosts: key mismatch"), "cloud.hostKeyChanged"));
    /* U5: what Get Info shows */
    char cl[201];
    cloud_clean_text("401 Unauthorized: <!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n<html><head>\n<title>401 Unauthorized</title>\n</head><body>\n<h1>Unauthorized</h1>", cl, sizeof cl);
    CHECK(!strchr(cl, '<') && !strchr(cl, '\n') && !strstr(cl, "  ") && strstr(cl, "401 Unauthorized") && strstr(cl, "Unauthorized"));
    char big[1000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    cloud_clean_text(big, cl, sizeof cl);
    CHECK(strlen(cl) == 200);
    cloud_clean_text("ñ\xe2\x82", cl, 3);                          /* never half a character */
    CHECK(!strcmp(cl, "ñ"));
    cloud_clean_text(NULL, cl, sizeof cl);
    CHECK(!cl[0]);
    /* the conflict rule: did the cloud's object change since it came down? */
    const char *h1[] = { "d41d8cd98f00b204e9800998ecf8427e" };
    CHECK(cloud_remote_changed("6", 6, -1, NULL, 0, 1) == 0 && cloud_remote_changed("6", 21, -1, NULL, 0, 1) == 1);
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08.123456789 +0000 UTC", 6, (double)cloud_rfc3339("2026-11-06T22:20:08Z"), NULL, 0, 1) == 0);
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08 +0000 UTC", 6, (double)cloud_rfc3339("2026-11-06T22:30:00Z"), NULL, 0, 1) == 1);
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08 +0000 UTC,d41d8cd98f00b204e9800998ecf8427e", 6, (double)cloud_rfc3339("2026-11-06T22:20:08Z"), h1, 1, 1) == 0);
    CHECK(cloud_remote_changed("6,ffffffffffffffffffffffffffffffff", 6, -1, h1, 1, 1) == 1);
    CHECK(cloud_remote_changed("", 6, -1, NULL, 0, 1) == -1 && cloud_remote_changed("abc", 6, -1, NULL, 0, 1) == -1 && cloud_remote_changed(NULL, 6, -1, NULL, 0, 1) == -1);
    /* N5: within the backend's precision a modtime is the same; a hash decides when both sides have one */
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08 +0000 UTC", 6, (double)cloud_rfc3339("2026-11-06T22:20:40Z"), NULL, 0, 60) == 0);
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08 +0000 UTC", 6, (double)cloud_rfc3339("2026-11-06T22:20:09Z"), NULL, 0, 1) == 0);
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08 +0000 UTC,d41d8cd98f00b204e9800998ecf8427e", 6, (double)cloud_rfc3339("2026-11-07T10:00:00Z"), h1, 1, 1) == 0);
    CHECK(cloud_remote_changed("6,2026-11-06 22:20:08 +0000 UTC,aaaa", 6, (double)cloud_rfc3339("2026-11-06T22:20:08Z"), h1, 1, 1) == 1);
    CHECK(!strcmp(cloud_failure_code("The attempted logon is invalid. This is either due to a bad username or authentication information."), "cloud.authFailed"));
    char cn[CLOUD_REL_MAX + 1];
    {   /* the server in the state (N4's count) */
        char sh[256];
        CHECK(cloud_server_of("sftp", "Files.Example.test", NULL, NULL, NULL, NULL, sh, sizeof sh) == 22 && !strcmp(sh, "files.example.test"));
        CHECK(cloud_server_of("sftp", "files.example.test", "2222", NULL, NULL, NULL, sh, sizeof sh) == 2222);
        CHECK(cloud_server_of("sftp", "files.example.test", "99999", NULL, NULL, NULL, sh, sizeof sh) == 0 && !sh[0]);
        CHECK(cloud_server_of("sftp", "files.example.test", "22x", NULL, NULL, NULL, sh, sizeof sh) == 0);
        CHECK(cloud_server_of("sftp", "a b", NULL, NULL, NULL, NULL, sh, sizeof sh) == 0);
        CHECK(cloud_server_of("sftp", "[::1]", NULL, NULL, NULL, NULL, sh, sizeof sh) == 22 && !strcmp(sh, "::1"));
        CHECK(cloud_server_of("ftp", "ftp.example.test", NULL, NULL, NULL, NULL, sh, sizeof sh) == 21);
        CHECK(cloud_server_of("ftp", "ftp.example.test", NULL, NULL, NULL, "true", sh, sizeof sh) == 990);
        CHECK(cloud_server_of("smb", "nas.example.test", NULL, NULL, NULL, NULL, sh, sizeof sh) == 445);
        CHECK(cloud_server_of("webdav", NULL, NULL, "https://dav.example.test/remote.php/dav", NULL, NULL, sh, sizeof sh) == 443 && !strcmp(sh, "dav.example.test"));
        CHECK(cloud_server_of("webdav", NULL, NULL, "http://user@127.0.0.1:8080/x", NULL, NULL, sh, sizeof sh) == 8080 && !strcmp(sh, "127.0.0.1"));
        CHECK(cloud_server_of("webdav", NULL, NULL, "http://[::1]:81", NULL, NULL, sh, sizeof sh) == 81 && !strcmp(sh, "::1"));
        CHECK(cloud_server_of("webdav", NULL, NULL, "http://h.example.test:x/", NULL, NULL, sh, sizeof sh) == 0 && !sh[0]);
        CHECK(cloud_server_of("webdav", NULL, NULL, "", NULL, NULL, sh, sizeof sh) == 0);
        CHECK(cloud_server_of("s3", NULL, NULL, NULL, "http://127.0.0.1:9000", NULL, sh, sizeof sh) == 9000);
        CHECK(cloud_server_of("s3", NULL, NULL, NULL, "s3.example.test", NULL, sh, sizeof sh) == 443 && !strcmp(sh, "s3.example.test"));
        CHECK(cloud_server_of("s3", NULL, NULL, NULL, NULL, NULL, sh, sizeof sh) == 0);
        CHECK(cloud_server_of("drive", "x", NULL, NULL, NULL, NULL, sh, sizeof sh) == 0 && !sh[0]);
        CHECK(cloud_server_of(NULL, "x", NULL, NULL, NULL, NULL, sh, sizeof sh) == 0);
    }
    CHECK(!cloud_conflict_name("notes.txt", 1, cn, sizeof cn) && !strcmp(cn, "notes (cloud copy).txt"));
    CHECK(!cloud_conflict_name("Docs/report.final.pdf", 2, cn, sizeof cn) && !strcmp(cn, "Docs/report.final (cloud copy 2).pdf"));
    CHECK(!cloud_conflict_name("Makefile", 1, cn, sizeof cn) && !strcmp(cn, "Makefile (cloud copy)"));
    CHECK(!cloud_conflict_name("a/.hidden", 1, cn, sizeof cn) && !strcmp(cn, "a/.hidden (cloud copy)"));
    CHECK(cloud_conflict_name("x.txt", 1, cn, 8) == -1);
    /* B8: a presigned link is long */
    {
        char u[4200] = "https://bucket.s3.example.test/file?X-Amz-Signature=";
        size_t l = strlen(u);
        while (l < 1200) u[l++] = 'a';
        u[l] = 0;
        CHECK(cloud_url_ok(u));
        while (l < 4100) u[l++] = 'b';
        u[l] = 0;
        CHECK(!cloud_url_ok(u));
    }
    CHECK(!strcmp(cloud_failure_code("Propfind \"http://127.0.0.1:40151/\": dial tcp 127.0.0.1:40151: connect: connection refused"), "cloud.unreachable"));
    CHECK(!strcmp(cloud_failure_code("530 Login incorrect."), "cloud.authFailed"));
    CHECK(!strcmp(cloud_failure_code("couldn't connect: smb: NT_STATUS_LOGON_FAILURE"), "cloud.authFailed"));
    CHECK(cloud_failure_code("directory not found") == NULL && cloud_failure_code("") == NULL && cloud_failure_code(NULL) == NULL);
}

static void t_signin(void) {
    char url[200], tok[4096], opts[1024];
    CHECK(cloud_auth_url("2026/11/06 14:51:51 NOTICE: Please go to the following link: http://127.0.0.1:53682/auth?state=vemwviceRJH-43KtIlsIGA", url, sizeof url) &&
          !strcmp(url, "http://127.0.0.1:53682/auth?state=vemwviceRJH-43KtIlsIGA"));
    CHECK(cloud_auth_url("NOTICE: Please go to the following link: http://127.0.0.1:53682/auth?state=abcdefgh12\n", url, sizeof url));
    CHECK(!cloud_auth_url("NOTICE: Please go to the following link: https://evil.example.test/auth?state=abcdefgh12", url, sizeof url));
    CHECK(!cloud_auth_url("NOTICE: Please go to the following link: http://127.0.0.1:53682/auth?state=abc\"<x>", url, sizeof url));
    CHECK(!cloud_auth_url("NOTICE: Please go to the following link: http://127.0.0.1:53682/auth?state=short", url, sizeof url));
    CHECK(!cloud_auth_url("NOTICE: Please go to the following link: http://127.0.0.1:536820/auth?state=abcdefgh12", url, sizeof url));
    CHECK(!cloud_auth_url("NOTICE: Waiting for code...", url, sizeof url) && !cloud_auth_url(NULL, url, sizeof url));
    CHECK(!cloud_auth_url("NOTICE: Please go to the following link: http://127.0.0.1:53682/auth?state=vemwviceRJH-43KtIlsIGA", url, 20));
    const char *out = "Paste the following into your remote machine --->\n{\"access_token\":\"sl.x\",\"token_type\":\"bearer\",\"refresh_token\":\"r1\",\"expiry\":\"2026-11-06T18:51:51Z\"}\n<---End paste\n";
    CHECK(cloud_auth_token(out, tok, sizeof tok, opts, sizeof opts) && tok[0] == '{' && strstr(tok, "\"refresh_token\":\"r1\"") && tok[strlen(tok) - 1] == '}');
    CHECK(!cloud_auth_token("Paste the following into your remote machine --->\nnot json\n<---End paste\n", tok, sizeof tok, opts, sizeof opts));
    CHECK(!cloud_auth_token("Paste the following into your remote machine --->\n{\"x\":1}\n<---End paste\n", tok, sizeof tok, opts, sizeof opts));
    CHECK(!cloud_auth_token("Paste the following into your remote machine --->\n{\"access_token\":\"a\nb\"}\n<---End paste\n", tok, sizeof tok, opts, sizeof opts));
    CHECK(!cloud_auth_token("", tok, sizeof tok, opts, sizeof opts) && !cloud_auth_token(NULL, tok, sizeof tok, opts, sizeof opts));
    CHECK(!cloud_auth_token(out, tok, 10, opts, sizeof opts));
    CHECK(cloud_auth_token(out, tok, sizeof tok, opts, sizeof opts) && !opts[0]);
    /* #71: started with a config blob, rclone prints base64 of every option the sign-in set (pCloud's host in Europe) */
    {
        const char *blob = "{\"token\":\"{\\\"access_token\\\":\\\"p.x\\\",\\\"token_type\\\":\\\"bearer\\\",\\\"expiry\\\":\\\"0001-01-01T00:00:00Z\\\"}\","
                           "\"hostname\":\"eapi.pcloud.com\",\"client_secret\":\"s\",\"token_url\":\"https://evil.example.test/\","
                           "\"config_x\":\"y\",\"ssh\":\"rm -rf\",\"Bad-Key\":\"v\",\"region\":\"a\\\"b\"}";
        char b64[2048], o2[2600];
        cloud_b64((const unsigned char *)blob, strlen(blob), b64);
        snprintf(o2, sizeof o2, "Paste the following into your remote machine --->\n%s\n<---End paste\n", b64);
        CHECK(cloud_auth_token(o2, tok, sizeof tok, opts, sizeof opts) && !strcmp(tok, "{\"access_token\":\"p.x\",\"token_type\":\"bearer\",\"expiry\":\"0001-01-01T00:00:00Z\"}"));
        CHECK(strstr(opts, ",\"hostname\":\"eapi.pcloud.com\"") && strstr(opts, ",\"region\":\"a\\\"b\"") && !strstr(opts, "client_secret") &&
              !strstr(opts, "token") && !strstr(opts, "config_") && !strstr(opts, "ssh") && !strstr(opts, "Bad"));
        for (char *e = strchr(b64, '='); e; e = strchr(b64, '=')) *e = 0;       /* RawStdEncoding: no padding */
        snprintf(o2, sizeof o2, "Paste the following into your remote machine --->\n%s\n<---End paste\n", b64);
        CHECK(cloud_auth_token(o2, tok, sizeof tok, opts, sizeof opts) && strstr(opts, "eapi.pcloud.com"));
        CHECK(!cloud_auth_token(o2, tok, sizeof tok, opts, 8));                 /* the options don't fit: nothing */
        const char *notok = "{\"hostname\":\"eapi.pcloud.com\"}";
        cloud_b64((const unsigned char *)notok, strlen(notok), b64);
        snprintf(o2, sizeof o2, "Paste the following into your remote machine --->\n%s\n<---End paste\n", b64);
        CHECK(!cloud_auth_token(o2, tok, sizeof tok, opts, sizeof opts) && !opts[0]);
        CHECK(!cloud_auth_token("Paste the following into your remote machine --->\n@@@@\n<---End paste\n", tok, sizeof tok, opts, sizeof opts));
    }
    CHECK(!strcmp(cloud_default_answer("config_refresh_token", "true"), "false"));
    CHECK(!strcmp(cloud_default_answer("config_shared_client_id", "false"), "true"));
    CHECK(!strcmp(cloud_default_answer("config_is_local", "true"), "false"));
    CHECK(!strcmp(cloud_default_answer("region", "eu"), "eu") && !strcmp(cloud_default_answer("x", NULL), "") && !strcmp(cloud_default_answer(NULL, "d"), "d"));

    /* #87: a work or school account's token is long (a JWT of a few KB): the blob still reads whole */
    {
        static char blob[12000], b64[17000], o3[17200], big[16384];
        char at[4001], rt[1501];
        memset(at, 'A', 4000); at[4000] = 0;
        memset(rt, 'r', 1500); rt[1500] = 0;
        snprintf(blob, sizeof blob, "{\"token\":\"{\\\"access_token\\\":\\\"%s\\\",\\\"token_type\\\":\\\"Bearer\\\",\\\"refresh_token\\\":\\\"%s\\\","
                 "\\\"expiry\\\":\\\"2026-11-09T15:00:00Z\\\",\\\"expires_in\\\":3599}\"}", at, rt);
        cloud_b64((const unsigned char *)blob, strlen(blob), b64);
        snprintf(o3, sizeof o3, "Paste the following into your remote machine --->\n%s\n<---End paste\n", b64);
        CHECK(cloud_auth_token(o3, big, sizeof big, opts, sizeof opts) && strstr(big, "\"refresh_token\":\"rrr") && strlen(big) > 5500);
    }
    /* #87: the drive of a browser sign-in is chosen again after rclone can't open it (OneDrive's driveid_final error) */
    {
        CloudChoices ch;
        memset(&ch, 0, sizeof ch);
        const char *drives[] = { "stale1", "b!real", "c" };
        CHECK(!strcmp(cloud_choose(&ch, "config_type", "onedrive", (const char *[]){ "onedrive", "sharepoint" }, 2), "onedrive"));
        CHECK(!cloud_choice_retry(&ch));                                          /* not a drive: an error after it stands */
        CHECK(!strcmp(cloud_choose(&ch, "config_driveid", "stale1", drives, 3), "stale1"));    /* rclone's default first */
        CHECK(cloud_choice_retry(&ch));                                           /* its error: go back */
        CHECK(!strcmp(cloud_choose(&ch, "config_type", "onedrive", (const char *[]){ "onedrive", "sharepoint" }, 2), "onedrive"));
        CHECK(!strcmp(cloud_choose(&ch, "config_driveid", "stale1", drives, 3), "b!real"));    /* the next one */
        CHECK(!strcmp(cloud_choose(&ch, "config_drive_ok", "true", (const char *[]){ "true", "false" }, 2), "true"));
        CHECK(!cloud_choice_retry(&ch));                                          /* an error after "Drive OK?": no retry */
        CHECK(!strcmp(cloud_choose(&ch, "config_driveid", "stale1", drives, 3), "c"));
        CHECK(cloud_choice_retry(&ch) == 0);                                      /* all three tried */
        CHECK(!strcmp(cloud_choose(&ch, "config_driveid", "stale1", drives, 3), "stale1") && !ch.last);
        /* one drive only: nothing to try again; a yes/no is never rotated */
        memset(&ch, 0, sizeof ch);
        CHECK(!strcmp(cloud_choose(&ch, "config_driveid", "only", (const char *[]){ "only" }, 1), "only") && !cloud_choice_retry(&ch));
        CHECK(!strcmp(cloud_choose(&ch, "config_refresh_token", "false", (const char *[]){ "true", "false" }, 2), "false") && !cloud_choice_retry(&ch));
        /* at most CLOUD_CHOICE_RETRIES times */
        memset(&ch, 0, sizeof ch);
        const char *many[] = { "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8" };
        int tries = 0;
        for (int i = 0; i < 20; i++) { cloud_choose(&ch, "config_driveid", "d1", many, 8); if (!cloud_choice_retry(&ch)) break; tries++; }
        CHECK(tries == CLOUD_CHOICE_RETRIES);
        CHECK(cloud_no_drive_msg("No drives found") && cloud_no_drive_msg("Failed to query available drives: /me/drives: 404; /me/drive: 404") &&
              cloud_no_drive_msg("Failed to query root for drive \"x\": itemNotFound") && !cloud_no_drive_msg("invalid_grant") && !cloud_no_drive_msg(NULL));
    }
}

static void t_options(void) {
    char r[64], a[16];
    long long s;
    CHECK(!cloud_bw_rate(0, 0, r, sizeof r) && !strcmp(r, "off"));
    CHECK(!cloud_bw_rate(512, 0, r, sizeof r) && !strcmp(r, "512Ki:off"));
    CHECK(!cloud_bw_rate(0, 2048, r, sizeof r) && !strcmp(r, "off:2048Ki"));
    CHECK(cloud_bw_rate(-1, 0, r, sizeof r) == -1 && cloud_bw_rate(0, CLOUD_BW_MAX + 1, r, sizeof r) == -1);
    CHECK(!cloud_expire("1d", a, sizeof a, &s) && !strcmp(a, "1d") && s == 86400);
    CHECK(!cloud_expire("30d", a, sizeof a, &s) && s == 30 * 86400);
    CHECK(!cloud_expire("never", a, sizeof a, &s) && !a[0] && !s);
    CHECK(cloud_expire("2d", a, sizeof a, &s) == -1 && cloud_expire("1d;rm", a, sizeof a, &s) == -1 && cloud_expire(NULL, a, sizeof a, &s) == -1);
}

static void t_meta(void) {
    CHECK(cloud_rfc3339("1970-01-01T00:00:00Z") == 0);
    CHECK(cloud_rfc3339("2026-11-06T14:51:51.00763413-03:00") == cloud_rfc3339("2026-11-06T17:51:51Z"));
    CHECK(cloud_rfc3339("2026-11-06T14:51:51+00:00") - cloud_rfc3339("2026-11-06T14:51:50Z") == 1);
    CHECK(cloud_rfc3339("2026-13-06T14:51:51Z") == -1 && cloud_rfc3339("garbage") == -1 && cloud_rfc3339("2026-11-06T14:51:51") == -1);
    CloudMeta m;
    const char *open = "{\"ModTime\":\"2026-11-06T14:51:51.00763413-03:00\",\"ATime\":\"2026-11-06T14:51:51.00979378-03:00\",\"Size\":2000000,\"Rs\":null,\"Fingerprint\":\"2000000\",\"Dirty\":false}";
    CHECK(!cloud_meta_parse(open, strlen(open), &m) && m.size == 2000000 && m.cached == 0 && !m.dirty && m.atime == cloud_rfc3339("2026-11-06T17:51:51Z"));
    const char *dirty = "{\"ModTime\":\"x\",\"ATime\":\"2026-11-06T14:51:51.182733935-03:00\",\"Size\":8,\"Rs\":[{\"Pos\":0,\"Size\":8}],\"Fingerprint\":\"6\",\"Dirty\":true}";
    CHECK(!cloud_meta_parse(dirty, strlen(dirty), &m) && m.size == 8 && m.cached == 8 && m.dirty);
    const char *part = "{\"Size\":100,\"Rs\":[{\"Pos\":0,\"Size\":10},{\"Pos\":50,\"Size\":20}],\"Dirty\":false}";
    CHECK(!cloud_meta_parse(part, strlen(part), &m) && m.cached == 30 && m.atime == -1);
    const char *over = "{\"Size\":10,\"Rs\":[{\"Pos\":0,\"Size\":1000}]}";
    CHECK(!cloud_meta_parse(over, strlen(over), &m) && m.cached == 10);
    CHECK(cloud_meta_parse("{\"Size\":-1}", 11, &m) == -1 && cloud_meta_parse("[]", 2, &m) == -1 && cloud_meta_parse("{", 1, &m) == -1);
    CHECK(cloud_meta_parse("{\"Size\":5,\"Rs\":\"x\"}", 19, &m) == -1);
}

static void t_status(void) {
    CloudMeta whole = { .size = 100, .cached = 100 }, part = { .size = 100, .cached = 40 }, dirty = { .size = 100, .cached = 100, .dirty = 1 };
    CloudLive l = { 0 };
    double pct;
    CHECK(!strcmp(cloud_file_status(NULL, &l, &pct), "cloud") && pct < 0);
    CHECK(!strcmp(cloud_file_status(&part, &l, &pct), "cloud"));
    CHECK(!strcmp(cloud_file_status(&whole, &l, &pct), "here"));
    l.kept = 1;
    CHECK(!strcmp(cloud_file_status(&whole, &l, &pct), "kept"));
    CHECK(!strcmp(cloud_file_status(&part, &l, &pct), "down") && pct == 40);
    CHECK(!strcmp(cloud_file_status(NULL, &l, &pct), "down") && pct == 0);
    l = (CloudLive){ .downloading = 1, .down_pct = 69 };
    CHECK(!strcmp(cloud_file_status(&part, &l, &pct), "down") && pct == 69);
    l = (CloudLive){ .queued = 1 };
    CHECK(!strcmp(cloud_file_status(&whole, &l, &pct), "up") && pct == 0);
    l = (CloudLive){ 0 };
    CHECK(!strcmp(cloud_file_status(&dirty, &l, &pct), "up"));
    l = (CloudLive){ .queued = 1, .uploading = 1, .up_pct = 91, .downloading = 1 };
    CHECK(!strcmp(cloud_file_status(&whole, &l, &pct), "up") && pct == 91);
    l.error = 1;
    CHECK(!strcmp(cloud_file_status(&whole, &l, &pct), "error"));
    CloudMeta empty = { .size = 0 };
    l = (CloudLive){ 0 };
    CHECK(!strcmp(cloud_file_status(&empty, &l, &pct), "here"));
}

static void t_evict(void) {
    CloudCand c[6] = {
        { 100, 50, 0, 0, 0, 0 },        /* 0: clean, old */
        { 200, 10, 0, 0, 0, 0 },        /* 1: clean, oldest */
        { 300, 5, 1, 0, 0, 0 },         /* 2: open */
        { 400, 1, 0, 1, 0, 0 },         /* 3: changed here */
        { 500, 2, 0, 0, 1, 0 },         /* 4: waiting to upload */
        { 600, 3, 0, 0, 0, 1 },         /* 5: kept */
    };
    CHECK(cloud_evict_why(&c[0]) == CE_OK && cloud_evict_why(&c[2]) == CE_OPEN && cloud_evict_why(&c[3]) == CE_PENDING);
    CHECK(cloud_evict_why(&c[4]) == CE_PENDING && cloud_evict_why(&c[5]) == CE_KEPT);
    CloudCand both = { 1, 1, 1, 1, 1, 1 };
    CHECK(cloud_evict_why(&both) == CE_KEPT);
    CHECK(!strcmp(cloud_skip_name(CE_OPEN), "open") && !strcmp(cloud_skip_name(CE_PENDING), "pending") && !strcmp(cloud_skip_name(CE_KEPT), "kept"));
    int pick[6];
    CHECK(cloud_evict_lru(c, 6, 150, pick) == 1 && pick[0] == 1);            /* oldest first */
    CHECK(cloud_evict_lru(c, 6, 250, pick) == 2 && pick[0] == 1 && pick[1] == 0);
    CHECK(cloud_evict_lru(c, 6, 1 << 30, pick) == 2);                         /* never the others, however much is needed */
    CHECK(cloud_evict_lru(c, 6, 0, pick) == 0);
}

/* ---- the state-backed parts, in a throwaway home with no rclone ---- */

static char home[512];

static void put(const char *rel, const char *data) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", home, rel);
    for (char *s = p + strlen(home) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { if (write(fd, data, strlen(data)) < 0) {} close(fd); }
}

static int exists(const char *rel) { char p[1024]; snprintf(p, sizeof p, "%s/%s", home, rel); return access(p, F_OK) == 0; }

static int nosec_get(const char *id, char **s) { (void)id; *s = NULL; return 1; }
static int nosec_set(const char *id, const char *l, const char *s) { (void)id; (void)l; (void)s; return 0; }
static int nosec_del(const char *id) { (void)id; return 0; }
static const CloudSecrets nosec = { nosec_get, nosec_set, nosec_del };

static void meta(const char *rel, long size, int dirty, const char *atime) {
    char m[512], p[700], d[700];
    snprintf(m, sizeof m, "{\"ModTime\":\"2026-11-06T14:51:51Z\",\"ATime\":\"%s\",\"Size\":%ld,\"Rs\":[{\"Pos\":0,\"Size\":%ld}],\"Fingerprint\":\"x\",\"Dirty\":%s}",
             atime, size, size, dirty ? "true" : "false");
    snprintf(p, sizeof p, ".cache/lordos/cloud/vfsMeta/cab12cd34/%s", rel);
    put(p, m);
    snprintf(d, sizeof d, ".cache/lordos/cloud/vfs/cab12cd34/%s", rel);
    char *data = malloc((size_t)size + 1);
    memset(data, 'x', (size_t)size);
    data[size] = 0;
    put(d, data);
    free(data);
}

static void t_state(void) {
    snprintf(home, sizeof home, "%s/cloud-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(home)) { CHECK(0); return; }
    setenv("LORDOS_CLOUD_RCLONE", "/nonexistent/rclone", 1);
    put(".config/lordos/cloud/accounts.json",
        "{\"options\":{\"bwUp\":0,\"bwDown\":0,\"autoFree\":false,\"freeBelowGb\":10},\"accounts\":["
        "{\"id\":\"cab12cd34\",\"name\":\"Work\",\"kind\":\"webdav\",\"created\":1},"
        "{\"id\":\"BAD\",\"name\":\"Bad\",\"kind\":\"webdav\"},"
        "{\"id\":\"c00000001\",\"name\":\"../escape\",\"kind\":\"webdav\"},"
        "{\"id\":\"c00000002\",\"name\":\"Other\",\"kind\":\"core\"}]}");
    put(".config/lordos/cloud/rclone.conf", "[cab12cd34]\ntype = webdav\nurl = https://dav.example.test/remote.php/dav\nvendor = other\n");
    put(".config/lordos/cloud/keep.json", "{\"cab12cd34\":[\"Kept\",\"../x\"]}");
    put(".config/lordos/cloud/links.json", "{\"links\":[{\"id\":\"cab12cd34\",\"path\":\"Share/a.txt\",\"url\":\"https://dav.example.test/s/abc\",\"expires\":0,\"made\":5},"
        "{\"id\":\"cab12cd34\",\"path\":\"../x\",\"url\":\"https://dav.example.test/s/x\",\"expires\":0,\"made\":5},"
        "{\"id\":\"cab12cd34\",\"path\":\"Old\",\"url\":\"https://dav.example.test/s/old\",\"expires\":1000,\"made\":5},"
        "{\"id\":\"cab12cd34\",\"path\":\"b\",\"url\":\"javascript:x\",\"expires\":0,\"made\":5}]}");
    meta("clean.bin", 4096, 0, "2026-11-06T14:51:51Z");
    meta("dirty.txt", 10, 1, "2026-11-06T14:51:51Z");
    meta("queued.txt", 10, 0, "2026-11-06T14:51:51Z");
    meta("open.txt", 10, 0, "2026-11-06T14:51:51Z");
    meta("Kept/k.bin", 10, 0, "2026-11-06T14:51:51Z");
    meta("Dir/one.bin", 10, 0, "2026-11-06T14:51:51Z");
    meta("Dir/Sub/two.bin", 10, 0, "2026-11-06T14:51:51Z");
    CHECK(cloud_start(home, &nosec) == 0);
    CHECK(!cloud_available());
    char *s = cloud_state_json();
    JDoc d;
    CHECK(s && !json_parse(s, strlen(s), &d, NULL, 0));
    if (s && d.root) {
        const JVal *as = json_get(d.root, "accounts");
        CHECK(as && as->type == J_ARR && as->len == 1);             /* only the valid account */
        const JVal *a = as ? as->kid : NULL;
        CHECK(a && !strcmp(json_str(a, "name"), "Work") && !strcmp(json_str(a, "kind"), "webdav"));
        const JVal *k = json_get(a, "kept");
        CHECK(k && k->len == 1 && !strncmp(k->kid->str, home, strlen(home)) && strstr(k->kid->str, "/Cloud/Work/Kept"));
        CHECK(json_get(d.root, "signin")->type == J_NULL && json_get(d.root, "recent")->type == J_ARR);
        CHECK(json_bool(json_get(d.root, "available") ? d.root : NULL, "available", 1) == 0);
        json_free(&d);
    }
    free(s);
    /* the links made from LordOS: only valid ones, not expired */
    s = cloud_links("cab12cd34");
    CHECK(s && strstr(s, "/Cloud/Work/Share/a.txt") && strstr(s, "https://dav.example.test/s/abc") && !strstr(s, "../x") && !strstr(s, "javascript") && !strstr(s, "/s/old"));
    free(s);
    s = cloud_links("nobody");
    CHECK(s && !strcmp(s, "[]"));
    free(s);
    s = cloud_website("cab12cd34");
    CHECK(s && !strcmp(s, "https://dav.example.test/remote.php/dav"));
    free(s);
    CHECK(cloud_website("nobody") == NULL);
    /* what JS can ask with no rclone */
    int areq = 0;
    s = cloud_add("webdav", "X", "{}", NULL, &areq);
    CHECK(s && strstr(s, "cloud.unavailable"));
    free(s);
    s = cloud_begin_signin("dropbox", "Y", NULL, NULL, NULL);
    CHECK(s && strstr(s, "cloud.unavailable"));
    free(s);
    s = cloud_rename("cab12cd34", "../up");
    CHECK(s && strstr(s, "cloud.badName"));
    free(s);
    s = cloud_rename("nobody", "Fine");
    CHECK(s && strstr(s, "cloud.notFound"));
    free(s);
    s = cloud_remove("nobody", 0);
    CHECK(s && strstr(s, "cloud.notFound"));
    free(s);
    s = cloud_set_options("{\"freeBelowGb\":1}");
    CHECK(s && strstr(s, "cloud.badOption"));
    free(s);
    s = cloud_set_options("{\"bwUp\":-5}");
    CHECK(s && strstr(s, "cloud.badOption"));
    free(s);
    s = cloud_set_options("{\"bwUp\":1024,\"freeBelowGb\":20}");
    CHECK(s && strstr(s, "\"ok\""));
    free(s);
    int req = 0;
    char p[1024];
    snprintf(p, sizeof p, "%s/Cloud/Work/../../.ssh/id", home);
    s = cloud_link(p, "7d", &req);
    CHECK(s && strstr(s, "cloud.badPath") && !req);
    free(s);
    snprintf(p, sizeof p, "%s/Cloud/Work/a.txt", home);
    s = cloud_link(p, "forever", &req);
    CHECK(s && strstr(s, "cloud.badExpire"));
    free(s);
    /* Delete in the cloud: only strictly inside an account, and only while it is mounted */
    {
        char d1[1024], d2[1024], d3[1024], d4[1024];
        snprintf(d1, sizeof d1, "%s/Cloud/Work", home);
        snprintf(d2, sizeof d2, "%s/Cloud/Work/../Work/x", home);
        snprintf(d3, sizeof d3, "%s/.ssh/id", home);
        snprintf(d4, sizeof d4, "%s/Cloud/Work/a.txt", home);
        const char *dp[] = { d1 }, *dp2[] = { d2 }, *dp3[] = { d3 }, *dp4[] = { d4 }, *dp5[] = { d4, d3 };
        int dreq = 0;
        s = cloud_delete(dp, 1, &dreq);
        CHECK(s && strstr(s, "cloud.badPath") && !dreq);              /* the account's own folder */
        free(s);
        s = cloud_delete(dp2, 1, &dreq);
        CHECK(s && strstr(s, "cloud.badPath") && !dreq);
        free(s);
        s = cloud_delete(dp3, 1, &dreq);
        CHECK(s && strstr(s, "cloud.badPath") && !dreq);
        free(s);
        s = cloud_delete(dp5, 2, &dreq);
        CHECK(s && strstr(s, "cloud.") && !dreq);                     /* one bad path refuses the whole request */
        free(s);
        s = cloud_delete(dp4, 1, &dreq);
        CHECK(s && strstr(s, "cloud.notRunning") && !dreq);           /* not mounted: nothing is deleted locally */
        free(s);
        s = cloud_delete(dp4, 0, &dreq);
        CHECK(s && strstr(s, "cloud.badArgument"));
        free(s);
        s = cloud_answer("c0a1b2c3d:x", "v", 1, &dreq);
        CHECK(s && strstr(s, "cloud.notFound") && !dreq);
        free(s);
        s = cloud_answer("cab12cd34:x", "v", 1, &dreq);
        CHECK(s && strstr(s, "cloud.noQuestion") && !dreq);
        free(s);
        s = cloud_answer("garbage", "v", 1, &dreq);
        CHECK(s && strstr(s, "cloud.badArgument"));
        free(s);
        s = cloud_measure("nobody", &dreq);
        CHECK(s && strstr(s, "cloud.notFound") && !dreq);
        free(s);
        s = cloud_providers(&dreq);
        CHECK(s && strstr(s, "cloud.unavailable") && !dreq);
        free(s);
    }
    /* status: merged from the metadata, the queue and Keep */
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find("cab12cd34");
    if (a) {
        a->q[0].name = strdup("queued.txt");
        a->q[0].size = 10;
        a->nq = 1;
    }
    pthread_mutex_unlock(&cl_mu);
    char p1[1024], p2[1024], p3[1024], p4[1024], p5[1024], p6[1024], p7[1024];
    snprintf(p1, sizeof p1, "%s/Cloud/Work/clean.bin", home);
    snprintf(p2, sizeof p2, "%s/Cloud/Work/dirty.txt", home);
    snprintf(p3, sizeof p3, "%s/Cloud/Work/queued.txt", home);
    snprintf(p4, sizeof p4, "%s/Cloud/Work/Kept/k.bin", home);
    snprintf(p5, sizeof p5, "%s/Cloud/Work/never.bin", home);
    snprintf(p6, sizeof p6, "%s/Cloud/Work/../Work/clean.bin", home);
    snprintf(p7, sizeof p7, "%s/Cloud/Work/Dir", home);
    const char *ps[] = { p1, p2, p3, p4, p5, p6, p7 };
    s = cloud_status(ps, 7);
    CHECK(s && !json_parse(s, strlen(s), &d, NULL, 0));
    if (s && d.root) {
        CHECK(!strcmp(json_str(json_get(d.root, p1), "st"), "here"));
        CHECK(!strcmp(json_str(json_get(d.root, p2), "st"), "up"));
        CHECK(!strcmp(json_str(json_get(d.root, p3), "st"), "up"));
        CHECK(!strcmp(json_str(json_get(d.root, p4), "st"), "kept"));
        CHECK(!strcmp(json_str(json_get(d.root, p5), "st"), "cloud"));
        CHECK(json_get(d.root, p6) == NULL);                      /* not a path under ~/Cloud */
        CHECK(!strcmp(json_str(json_get(d.root, p7), "st"), "cloud"));
        json_free(&d);
    }
    free(s);
    /* #71: a kept folder whose files are still coming down stays "down" between one file and the next (and the folder
     * that holds it); once the keeper got them all, kept again */
    {
        char pk[1024], pw[1024];
        snprintf(pk, sizeof pk, "%s/Cloud/Work/Kept", home);
        snprintf(pw, sizeof pw, "%s/Cloud/Work", home);
        const char *pks[] = { pk, pw, p4 };
        int at = -1;
        pthread_mutex_lock(&cl_mu);
        Acc *w = cl_find("cab12cd34");
        for (int i = 0; w && i < w->nkeep; i++) if (!strcmp(w->keep[i], "Kept")) at = i;
        if (at >= 0) w->keep_busy[at] = 1;
        pthread_mutex_unlock(&cl_mu);
        CHECK(at >= 0);
        s = cloud_status(pks, 3);
        CHECK(s && !json_parse(s, strlen(s), &d, NULL, 0));
        if (s && d.root) {
            CHECK(!strcmp(json_str(json_get(d.root, pk), "st"), "down"));
            CHECK(!strcmp(json_str(json_get(d.root, pw), "st"), "up"));               /* the account's folder: a queued upload wins */
            CHECK(!strcmp(json_str(json_get(d.root, p4), "st"), "kept"));        /* a file already here stays kept */
            json_free(&d);
        }
        free(s);
        pthread_mutex_lock(&cl_mu);
        if (w && at >= 0) w->keep_busy[at] = 0;
        pthread_mutex_unlock(&cl_mu);
        s = cloud_status(pks, 2);
        CHECK(s && !json_parse(s, strlen(s), &d, NULL, 0));
        if (s && d.root) {
            CHECK(!strcmp(json_str(json_get(d.root, pk), "st"), "kept"));
            json_free(&d);
        }
        free(s);
    }
    /* Free Up Space on everything: only the clean files go */
    char top[1024];
    snprintf(top, sizeof top, "%s/Cloud/Work", home);
    const char *tp[] = { top };
    s = cloud_free(tp, 1);
    CHECK(s && !json_parse(s, strlen(s), &d, NULL, 0));
    if (s && d.root) {
        CHECK(json_num(d.root, "freed", 0) >= 4096);
        const JVal *sk = json_get(d.root, "skipped");
        int kept = 0, pend = 0;
        for (const JVal *x = sk ? sk->kid : NULL; x; x = x->next) {
            kept += !strcmp(json_str(x, "reason"), "kept");
            pend += !strcmp(json_str(x, "reason"), "pending");
        }
        CHECK(kept == 1 && pend == 2);
        json_free(&d);
    }
    free(s);
    CHECK(!exists(".cache/lordos/cloud/vfs/cab12cd34/clean.bin") && !exists(".cache/lordos/cloud/vfsMeta/cab12cd34/clean.bin"));
    CHECK(!exists(".cache/lordos/cloud/vfs/cab12cd34/Dir/Sub/two.bin") && !exists(".cache/lordos/cloud/vfs/cab12cd34/open.txt"));
    CHECK(exists(".cache/lordos/cloud/vfs/cab12cd34/dirty.txt") && exists(".cache/lordos/cloud/vfs/cab12cd34/queued.txt"));
    CHECK(exists(".cache/lordos/cloud/vfs/cab12cd34/Kept/k.bin") && exists(".cache/lordos/cloud/vfsMeta/cab12cd34/Kept/k.bin"));
    /* a file open right now: rclone says something is in use, and a process of this user has it open */
    meta("busy.bin", 64, 0, "2026-11-06T14:51:51Z");
    put("Cloud/Work/busy.bin", "");
    snprintf(p, sizeof p, "%s/Cloud/Work/busy.bin", home);
    int fd = open(p, O_RDWR);
    CHECK(fd >= 0);
    pthread_mutex_lock(&cl_mu);
    if (a) a->in_use = 1;
    pthread_mutex_unlock(&cl_mu);
    const char *bp[] = { p };
    s = cloud_free(bp, 1);
    CHECK(s && strstr(s, "\"reason\":\"open\"") && strstr(s, "\"freed\":0"));
    free(s);
    CHECK(exists(".cache/lordos/cloud/vfs/cab12cd34/busy.bin"));
    close(fd);
    s = cloud_free(bp, 1);
    CHECK(s && !strstr(s, "\"reason\":\"open\"") && !strstr(s, "\"freed\":0"));
    free(s);
    CHECK(!exists(".cache/lordos/cloud/vfs/cab12cd34/busy.bin"));
    /* Keep and Unkeep: the files on disk */
    snprintf(p, sizeof p, "%s/Cloud/Work/Dir", home);
    const char *kp[] = { p, "/etc/passwd" };
    cloud_keep(kp, 2, 1);
    char *kj = NULL;
    {
        char kf[1024];
        snprintf(kf, sizeof kf, "%s/.config/lordos/cloud/keep.json", home);
        FILE *f = fopen(kf, "r");
        static char b[4096];
        size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
        if (f) fclose(f);
        b[n] = 0;
        kj = b;
        struct stat st;
        CHECK(!stat(kf, &st) && (st.st_mode & 0777) == 0600);
    }
    CHECK(kj && strstr(kj, "\"Dir\"") && strstr(kj, "\"Kept\"") && !strstr(kj, "passwd") && !strstr(kj, "../"));
    cloud_keep(kp, 1, 0);
    s = cloud_state_json();
    CHECK(s && !strstr(s, "/Cloud/Work/Dir\""));
    free(s);
    struct stat st;
    snprintf(p, sizeof p, "%s/.config/lordos/cloud", home);
    CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0700);
    snprintf(p, sizeof p, "%s/.config/lordos/cloud/accounts.json", home);
    CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
}

/* 7.3: the mount keeps a listing an hour and polls changes; a folder shown is listed again only when old, on a provider
 * without change notifications and outside a kept folder; the table of folders listed in this mount */
static void t_browse(void) {
    char o[400];
    CHECK(!cloud_mount_opts(o, sizeof o) && o[0] == ',');
    CHECK(strstr(o, "\"CacheMode\":\"full\"") && strstr(o, "\"CacheMaxSize\":-1") && strstr(o, "\"WriteBack\":5000000000"));
    CHECK(strstr(o, "\"DirCacheTime\":3600000000000,") && strstr(o, "\"PollInterval\":60000000000}"));
    CHECK(strstr(o, "\"mountOpt\":{\"AllowNonEmpty\":false}") && o[strlen(o) - 1] == '}');
    char body[480];
    snprintf(body, sizeof body, "{\"fs\":\"c0000001:\",\"mountPoint\":\"/h/Cloud/W\"%s}", o);
    JDoc d;
    CHECK(!json_parse(body, strlen(body), &d, NULL, 0) && d.root);
    if (d.root) {
        const JVal *v = json_get(d.root, "vfsOpt");
        CHECK(v && json_num(v, "DirCacheTime", 0) == 3600e9 && json_num(v, "PollInterval", 0) == 60e9 && json_num(v, "CacheMaxAge", 0) > 3e18);
        CHECK(json_get(d.root, "mountOpt") != NULL);
        json_free(&d);
    }
    CHECK(cloud_mount_opts(o, 40) == -1);
    /* the provider sends changes, or the keeper lists it, or Files just listed it: nothing; else after 5 minutes */
    long long now = 10LL * 3600 * 1000;
    CHECK(!cloud_browse_due(-1, now, 0, 0));
    CHECK(!cloud_browse_due(now - 299 * 1000, now, 0, 0));
    CHECK(cloud_browse_due(now - 300 * 1000, now, 0, 0));
    CHECK(!cloud_browse_due(now - 3000 * 1000, now, 1, 0));
    CHECK(!cloud_browse_due(now - 3000 * 1000, now, 0, 1));
    /* keys: account, mount and folder all count, and their bounds */
    unsigned long long k = cloud_seen_key("c1", 1, "a/b");
    CHECK(k == cloud_seen_key("c1", 1, "a/b"));
    CHECK(k != cloud_seen_key("c1", 2, "a/b") && k != cloud_seen_key("c2", 1, "a/b") && k != cloud_seen_key("c1", 1, "a/c"));
    CHECK(cloud_seen_key("c1a", 1, "") != cloud_seen_key("c1", 1, "a") && cloud_seen_key(NULL, 0, NULL) == cloud_seen_key("", 0, ""));
    static CloudSeen s;
    memset(&s, 0, sizeof s);
    CHECK(cloud_seen_at(&s, k) == -1);
    cloud_seen_put(&s, k, 5);
    CHECK(cloud_seen_at(&s, k) == 5 && s.n == 1);
    cloud_seen_put(&s, k, 9);
    CHECK(cloud_seen_at(&s, k) == 9 && s.n == 1);
    /* full: the oldest goes, the rest stay */
    for (int i = 0; i < CLOUD_SEEN_MAX + 10; i++) cloud_seen_put(&s, 1000 + (unsigned long long)i, 100 + i);
    CHECK(s.n == CLOUD_SEEN_MAX && cloud_seen_at(&s, k) == -1 && cloud_seen_at(&s, 1000) == -1);
    CHECK(cloud_seen_at(&s, 1000 + CLOUD_SEEN_MAX + 9) == 100 + CLOUD_SEEN_MAX + 9 && cloud_seen_at(&s, 1010) == 110);
}

int main(void) {
    t_browse();
    t_methods();
    t_http();
    t_paths();
    t_kinds();
    t_rules();
    t_refused();
    t_signin();
    t_options();
    t_meta();
    t_status();
    t_evict();
    t_state();
    printf("cloud-test: %d passed, %d failed\n", pass, failc);
    return failc ? 1 : 0;
}
