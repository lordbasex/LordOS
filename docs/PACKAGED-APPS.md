# Packaged apps and companions

A native app that a `.deb` installs, signed by its author, and which may run one helper program of its own. It's stage 2 of [GO-LINK.md](GO-LINK.md), general, and knows
nothing about go-link. It's the only way third-party code gets to run a binary from a native app. Code: `server/src/sec/pkgapp.c`, `sig_verify_tree` in `sec/sign.c`,
`sandbox_exec_companion` in `sec/sandbox.c`, `server/src/app/companion.c`, `sec/statusitem.c`, `sdk/companion.js`, `sdk/appstatus.jsx`.

## The third root

`/usr/lib/lordos-apps/<id>/`, with the layout of an unpacked `.lpk`, made by `node tools/lpk.mjs tree <folder> <output> --key key.json --companion <binary>`.

- A folder counts only if root owns it and its root, and no one else can write them. It's never a link. Every entry under it is a regular file or a folder, root's, not writable by
  group or others, and never set-id.
- It's looked for after `assets/sysapps` and before the home folder, so a home `.lpk` can't shadow it, and installing one with the same id is refused. It's never `system: true` and
  never in `code-manifest.json`, and it runs as `lord-server --app <id>` in the usual sandbox, under the id `lpk:<id>`.
- **At every load,** before the sandbox goes on: `firma.json` over `contenido.json`, every file hashed, none missing or extra, and the key must be a **Verified author's** in LordOS's
  signed catalog, not revoked. A key the catalog doesn't list, no catalog, and the official key all fail. It's installed and removed by dpkg, so Remove isn't offered.

A refused app doesn't open, and the reason is in `~/.cache/lordos/session.log`.

## What only this root may declare

| Manifest | Meaning |
|---|---|
| `"companion": "bin/engine"` | one binary, a path inside the package, listed in `contenido.json` |
| `"permissions": ["screen-share", "remote-input"]` | use the [screen-sharing API](SCREEN-SHARING.md) through the companion |

`tools/lpk.mjs` refuses these in an `.lpk`. A home package that has them anyway is ignored: lord-server decides by where it found the app, never by what the app or its manifest says.

## The companion

The app host asks when the app starts (`BOP_COMPANION`), after asking for `network` if the manifest declares it. It sends no path and no argument, and the id comes from the connection.
lord-server keeps the verified root in its own table, forks, and execs itself as `lord-server --companion <id> <0|1>`, without waiting. That new process, with one thread and nothing of the
compositor's:

1. refuses to run as root, and stops at once without lord-server's socket as fd 3;
2. repeats the whole check, keeping the descriptor the binary was hashed through;
3. opens `~/Downloads` with no link followed, joins `app-lordos-lpk-<id>.scope` with the app's limits (one core, 512 MB, shared), and refuses if that fails in a real session;
4. applies the sandbox, and runs that descriptor with `execveat(AT_EMPTY_PATH)`.

Any failure stops there: nothing of the package runs, and the app hears why on fd 3. A changed byte gives `sig.fileModified`.

**Descriptors:** 3, a `SOCK_SEQPACKET` socket to the app, one JSON text per message, 64 KB at most. 4, a connection to the sharing API tied to `lpk:<id>`, only if the manifest declares
`screen-share`. 0, 1 and 2 are `/dev/null`. Nothing else, and the environment is `HOME`, `LANG` and `LORD_APP_ID`.

**It ends with the app:** when the app's broker connection goes, lord-server sends SIGKILL and reaps it; the end of fd 3 is there for a clean exit; and `PR_SET_PDEATHSIG` covers lord-server
dying. A companion that keeps dying is started 5 times at most per run of the app.

**Firewall:** in the app's scope, so the Monitor and the firewall see one app. `FW_R_NO_FILES` refuses the start, since it reads the home folder.

## Its sandbox

Everything or it doesn't start: a kernel with Landlock below ABI 3, or any rule that fails, means no exec. Applied before exec, because Go can't apply it from inside.

| Limit | Value |
|---|---|
| Identity | no capabilities, `no_new_privs`, not root |
| Read | the home folder, the app's folder; with network, `/etc/ssl`, `/etc/resolv.conf`, `/run/systemd/resolve` |
| Write | `~/Downloads`: files and folders, rename, delete, truncate. No links, sockets or fifos |
| Execute | only the binary |
| Network | with `network` declared and allowed: `AF_INET`, `AF_INET6` and netlink's route family. It connects and binds; `listen` and `accept` are refused |
| Everything else | a syscall allowlist of its own, EPERM for the rest |

**Never** a new `AF_UNIX` socket or `socketpair`: Landlock doesn't filter `connect` on named sockets. Refused too: `fork` and any `clone` that isn't a thread, `execve`, `memfd_create`, `ioctl`,
`prctl`, `ptrace`, `unshare`, `kill`, signals and `prlimit` on another process, and `chmod`, `chown`, `utimensat` and `setxattr`, which Landlock doesn't cover and which would otherwise reach the
whole home folder. `execveat` stays in the filter because the filter is on before the exec, and it names one descriptor. Landlock lets only the binary run, and without `memfd_create` there's no file
out of Landlock's reach to run.

**What it can do if compromised:** read what the user can read in the home folder and send it out, write in Downloads, and, while a share runs, what fd 4 grants. It can't change anything else.

## An item by the clock

For any app with the `status-item` permission, packaged or not. An isolated app has no code in the shell, so it describes the item:

```js
statusItem.set({ icon: 'icon.png', title: 'Sharing', alert: true, menu: [{ id: 'open', label: 'Open' }, { line: true }, { id: 'quit', label: 'Quit', off: false }] });
statusItem.onSelect((id) => …);
```

The system prompt asks the first time. lord-server reads what came and writes a new JSON text with only the known fields, each escaped. The icon must be a `.png` of the app's own package, by a checked
relative path. Texts are one line of valid UTF-8, 64 bytes at most, with no control or bidirectional marks. A menu has 12 entries at most. One update per 100 ms. The shell draws it, and a choice comes back as an id.
It leaves when the app ends or the permission is revoked in Settings → Privacy. An app gets one.

## Uninstalling

`packages.remove` forgets a home app's grants. A packaged app is removed with dpkg, behind the shell's back, so each session start forgets the grants of any app that's no longer there. A list that couldn't
be read forgets nothing.

## What was tested

On VM 116 (amd64, kernel 6.12, 2026-11-01), with a static Go binary built with `CGO_ENABLED=0`, in isolated headless sessions. `make -C server test` includes `pkgapp-test`. `tests/companion_e2e.sh` runs
`build/lord-server-pkgtest`, a test binary that takes its official key and root from the environment, on a root-owned folder, with throwaway keys from `tests/companion/fixture.sh`.

- `pkgapp-test`, 52 checks: every way the folder can differ from what was signed, keys, revocation, and status item descriptions, good and hostile.
- `companion_e2e.sh`, 100 checks passed: the app opened by Open at Login. **65 checks from inside the sandbox:** every limit above tried, and what must work: files in Downloads, TLS roots, interfaces through netlink,
  and a Pion data channel between two peers in the process over DTLS. A mistake in the seccomp filter's jumps that let everything through was found this way. fd 3 both ways, and H.264 frames through fd 4.
  Outside it: `no_new_privs`, seccomp, no capabilities, one process, killed with the app, no zombie. Refused with nothing run: one changed byte, no Verified author, a revoked key, no catalog, a folder of the user's, a
  group-writable folder, as root, Downloads as a link. No grant on fd 4 gives `no_person`. The same manifest in a home folder runs as an app with no companion. A Linux app in an `lpk-` scope gets `no_identity`.

**Not tested:** arm64; a real `lord-sessiond` placing the companion in its scope (those sessions were refused placement, as a development session is, and it isn't fatal there); limits of one core and 512 MB with a real engine;
a real `.deb`; a kernel below ABI 3; seen on screen: the item by the clock and its menu, whose choice reaching the app wasn't run; the first-use prompts for `network` and `status-item`; the admin action behind forgetting grants, from a real shell.
