# Cloud end-to-end bench

Real headless LordOS sessions on a throwaway Debian 13 VM (VM 116), driven through the same SDK the interface uses, against
real servers on loopback: OpenSSH (password, key, key with a passphrase), Apache WebDAV (HTTP, HTTPS with a self-signed
certificate, HTTPS with a certificate from the bench's own CA), MinIO (HTTP and TLS), `rclone serve s3`, vsftpd and Samba.
Test only: nothing here ships, and every credential below is a throwaway one for this bench.

## Files

| File | What |
|---|---|
| `servers.sh` | the servers (as root): `setup`, `start`, `stop`, `start-one`/`stop-one NAME` (offline cases), `status`, `purge` |
| `session.sh` | a session per Linux user (as root): `install TREE` (to `/opt/lordos-e2e`, root-owned, so `lord-vault` runs), `start USER` (`NOKEYRING=1` leaves the keyring locked), `unlock USER`, `stop USER` (a logout: SIGTERM, then the unit), `unmount USER`, `purge USER` |
| `driver.js`, `build-e2e.mjs` | a test-only theme bundle: the real default theme plus a driver that runs `~/.e2e/cmd-N.js` (the body of an async function with `cloud`, `cloudState`, `wm`, `lord`, `sleep`, `log`, `replies`) and writes `~/.e2e/out-N.json`. Also writes the code manifest that lists it (lord-server loads only listed code) |
| `lc.py` | `lc.py USER JS|- [TIMEOUT]` (as root): runs JS in USER's session, `lib.js` prepended |
| `lib.js` | helpers: `S()`, `A(name)`, `brief`, `until`, `mounted`, `C(rel)`, `call` |
| `ui.py` | the remote desktop protocol (as root): clicks, keys, wheel, `grab:FILE.png`, `keyring:PASSWORD` (RP_KEYRING, as lord-sessiond after a login). Typing goes through the session's keyboard layout (LA on VM 116: `:` is `combo:42+52`, `/` is `combo:42+8`) |
| `cases/files.sh`, `cases/files.js` | Files' operations on one account through `lord.fs` (write, status up → here, rename, mkdir, move, copy in, copy out, list), the server's side checked, then `cloud.delete` and no `.Trash` left |
| `cases/adds.js`, `cases/errors.js` | every good account (trust questions answered) and every add that must fail (wrong password, key, passphrase, port, host, address, S3 secret, options that run programs or name files), with what's left behind |
| `cases/check.js` | Check This Account with Settings' own env, and its report |
| `push.sh` | from the Mac: rebuild the bundle and copy the bench (`full`: the tree, `make`, install) |

## Run

```sh
sh tools/cloud/e2e/push.sh full                                   # on the Mac
ssh VM 'sudo sh ~/LordOS-cloud-e2e/tools/cloud/e2e/servers.sh setup && sudo sh ~/LordOS-cloud-e2e/tools/cloud/e2e/servers.sh start'
ssh VM 'sudo sh ~/LordOS-cloud-e2e/tools/cloud/e2e/session.sh start e2ea'
ssh VM 'sudo python3 -I ~/LordOS-cloud-e2e/tools/cloud/e2e/lc.py e2ea "await cloud.providers(); return await cloud.add(\"webdav\", \"Dav\", { url: \"http://127.0.0.1:8080/dav/\", user: \"davuser\", pass: \"E2e-dav-0001\" })"'
ssh VM 'sudo sh ~/LordOS-cloud-e2e/tools/cloud/e2e/cases/files.sh e2ea Dav /srv/e2e/dav/files'
```


MinIO: dl.min.io answers 410 for community binaries now, so the bench builds it with Go (go.dev's archive checked against
the SHA-256 go.dev publishes; the modules checked by the Go checksum database): `go install
github.com/minio/minio@RELEASE.2025-10-15T17-29-55Z` and `github.com/minio/mc@latest`, installed in `/usr/local/lib/lordos-e2e/`.

## Throwaway credentials (loopback only)

| Server | Address | Credentials |
|---|---|---|
| SSH/SFTP, password | 127.0.0.1:2222 | `e2epw` / `E2e-pw-0001` |
| SSH/SFTP, key | 127.0.0.1:2222 | `e2ekey`, `/etc/lordos-e2e/keys/id_plain` (ed25519, no passphrase), `id_pass` (passphrase `E2e-key-pass-0001`); `id_other` is not authorized |
| WebDAV | http://127.0.0.1:8080/dav/, https://127.0.0.1:8443/dav/ (self-signed), https://127.0.0.1:8444/dav/ (bench CA, in the system's trust store) | `davuser` / `E2e-dav-0001` |
| MinIO | http://127.0.0.1:9000, https://127.0.0.1:9443 | root `e2eminio` / `E2e-minio-root-0001`; app key `e2eappkey` / `E2e-minio-app-secret-0001`; buckets `e2e-alpha`, `e2e-beta` |
| rclone serve s3 | http://127.0.0.1:9100 | `e2ers3key` / `E2e-rs3-secret-0001`; buckets `bucket-a`, `bucket-b` |
| FTP | 127.0.0.1:2121 | `e2epw` / `E2e-pw-0001` |
| SMB | 127.0.0.1:445, share `e2e` | `e2epw` / `E2e-pw-0001` |
| Client sessions | users `e2ea`, `e2eb` | login and keyring password `E2e-client-<user>-0001` |
