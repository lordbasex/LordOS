# Passwords and the system keyring (M9)

**Passwords** ("Contraseñas" in Spanish, "Senhas" in Portuguese) is the LordOS password manager.
The same machinery is also the **system keyring**: native apps keep their secrets there through
`lord.keyring` / the SDK `keyring`, and Linux apps (Chromium, Firefox, `secret-tool`, anything
using libsecret) through our own implementation of the freedesktop **Secret Service** D-Bus API.
No gnome-keyring, kwallet, libsecret, libdbus or sd-bus is used by LordOS.

## Screenshots (VM 116, real `lord-sessiond` session, user `pwtest`)

The current look (polish pass, VM 118) is in [`docs/polish/passwords-comparison.png`](polish/passwords-comparison.png)
with every `passwords-before-*` / `passwords-after-*` shot (light, dark, RTL, minimum size, locked,
editor, generator, first run); the shots below are from the first version.

| | |
|---|---|
| ![Create the vault](passwords-create.png) | ![Unlock with the master password](passwords-unlock.png) |
| Creating the vault: choose how it unlocks (password, FIDO2 key, key file, password + key file) | Locked vault: one tab per unlock method present in the file |
| ![Internet item with TOTP](passwords-internet.png) | ![2FA codes](passwords-2fa.png) |
| An *Internet* item: website, user, hidden password (reveal/copy), live 2FA code with countdown, notes, reminder "change every 90 days" | All 2FA codes with their countdown; the TOTP secret never leaves `lord-vault` |
| ![Wi-Fi QR](passwords-wifi-qr.png) | ![Identity document with photo](passwords-identity.png) |
| Wi-Fi: the QR code to join from a phone (`WIFI:T:WPA;S:…;P:…;;`) is generated inside `lord-vault` | *Identity and documents*: encrypted photo attachment previewed (decoded and downscaled inside `lord-vault`) |
| ![Finance](passwords-finance.png) | ![Card](passwords-card.png) |
| *Finance*: password and coordinates card revealed on request; the "change every 90 days" reminder is overdue | *Cards*: the list shows only the last 4 digits; number, CVV and PIN stay hidden |
| ![Servers/SSH](passwords-ssh-key.png) | ![Crypto](passwords-crypto-wallet.png) |
| *Servers/SSH* with the private key (multi-line hidden field) and host fingerprint | *Crypto*: extra warning about the seed phrase |
| ![License with its attached file](passwords-software-license.png) | |
| *Licenses*: the licence key plus the attached licence file, opened from the encrypted attachment | |
| ![Security report](passwords-security-check.png) | ![Generator](passwords-generator.png) |
| Security report: weak, reused, old and breached (offline) passwords | Password generator |
| ![Passphrase generator](passwords-generator-passphrase.png) | ![Import KeePass](passwords-import.png) |
| Passphrases from the EFF word list (12.9 bits per word) | Importing a real KeePass KDBX 4 file with our own reader: 4 entries, 3 folders become categories |
| ![Unlock with a USB key file](passwords-unlock-usb-key.png) | ![Unlock with a FIDO2 key](passwords-unlock-security-key.png) |
| Unlocking with a key file on a USB stick (`/media/pwtest/PENDRIVE`) | Unlocking with a FIDO2 security key (here the software authenticator, see "Testing") |
| ![Vault settings](passwords-settings.png) | ![USB permission prompt](passwords-security-key-permission.png) |
| Unlock methods: password, recovery key, FIDO2 key and key file side by side; each wraps the same vault key | Using the security key goes through the system permission prompt (the per-app `usb` permission); only `lord-server` opens `/dev/hidrawN` |
| ![Chromium offers to save](passwords-browser-save.png) | ![Chromium Safe Storage in the session keyring](passwords-browser-keyring.png) |
| Chromium (Wayland, `--password-store=gnome-libsecret`) saving a password | …its "Chromium Safe Storage" key lands in our Secret Service, visible in *Session keyring* |
| ![Chromium autofills after a restart](passwords-browser-autofill.png) | ![Protected window in a capture](passwords-protected-capture.png) |
| After restarting Chromium it decrypts and autofills the saved login: the key came back from our service | The Passwords window comes out black in LordOS screenshots, recordings and the remote desktop |

| ![Categories tree and tags](passwords-categories.png) | ![The real screen at the same moment](passwords-real-screen.png) |
|---|---|
| Main window: user categories as a tree (Trabajo → Clientes, Hogar → Inventario, imported KeePass folders), tags with colours | The real monitor (QEMU screendump) when the black LordOS screenshot above was taken: only captures are blanked |

## Architecture: where the secrets live

```
 Passwords app process                       Settings → Network, Mail, … (sdk keyring)
 (lord-server --app passwords, sandboxed;            │ lord.vault.request(json, files)
  apps/passwords, src/ui/js_vault.c app mode)        │
        │ own channel (vc_attach_app)                ▼
        │ LORD_VAULT_FD                     lord-server  (src/vault/vclient.c, src/ui/js_vault.c)
   - spawns lord-vault (fixed path, no args, minimal env), one per session
   - opens files the user picked (validated like lord.fs) and passes only fds
   - opens /dev/hidrawN for FIDO2 after the `usb` permission, passes the fd
   - connects the session D-Bus socket and passes it (Secret Service)
   - screen lock / user switch → "lock-all"; login → session password (RP_KEYRING)
        │ SOCK_SEQPACKET, JSON messages, SCM_RIGHTS (src/vault/vwire.c)
        ▼
 lord-vault  (src/vault/vault.c, keyring.c, …) — sandbox_apply_helper:
   no network, no new sockets, no exec, threads only, Landlock: only the vault folder (fd)
   - XChaCha20-Poly1305 + Argon2id (Monocypher), the .lvault format
   - decrypted vaults in the secret arena (mlock, MADV_DONTDUMP, wiped on lock)
   - search, security report, TOTP, generator, QR encode/decode, importers, KDBX reader
   - CTAP2 client over the hidraw fd (thread waits for the touch)
   - Secret Service on the inherited bus socket (own D-Bus code)
```

**Why this split.** Everything that touches plaintext vault data, parses untrusted files (KDBX,
CSV, JSON, images with QR codes) or talks to the security key runs in one small helper that cannot
open the network, sockets or paths, the same model as `lord-doc` and `lord-media`
(`docs/PRIVACY.md`, "Decoding helpers"). `lord-server` (the compositor) only relays: it never holds
the vault key and never parses the vault. The UI gets just what it shows:

- the list view gets titles, user names, URLs, tags and flags, **never** a secret;
- an open item comes with hidden fields as `{"has": true}`; a value travels only when the user
  clicks reveal, copy or edits it;
- TOTP secrets never leave `lord-vault`: the UI receives the 6–8 digit code and the seconds left;
- the Wi-Fi QR is built from the item inside `lord-vault`; the UI receives a bitmap.

### The UI runs in its own sandboxed process (app mode)

The Passwords window is not drawn by the session's JavaScript context: it is its own process,
`lord-server --app passwords` (a Wayland client of the compositor with Landlock + seccomp and no
network, like an installed `.lpk`). Revealed values, edited fields and the decrypted list live only
in that process and in `lord-vault`; `lord-server`'s QuickJS heap never sees them.

| Piece | How |
|---|---|
| System package | `tools/build-theme.mjs` builds every built-in app whose manifest says `"process": "isolated"` into `assets/sysapps/<id>/` (`app.js`, `manifest.json`, `locales/`). `app_package_dir()` (`src/app/appspawn.c`) prefers that folder (root- or user-owned, not world-writable) over a user `.lpk` with the same id, so a package cannot shadow it. |
| Vault channel | `app_spawn()` calls `vc_attach_app()` **only** for the system `passwords` package and hands the socket to the child as `LORD_VAULT_FD`. In the app, `js_vault_register_app()` exposes the same `lord.vault` API over that socket (non-blocking `vw_feed` polling, `lord.vault.appMode = true`). The relayed JSON is wiped after use. |
| Files | The system's own open/save dialog (powerbox, drawn by `lord-server`) picks the file; the app gets only the fd and passes it on to `lord-vault` (`request(json, { fds })`). Key files, attachments, import, export, the breach list and QR images all go this way. |
| FIDO2 | `BOP_VAULT_FIDO`: after the user allows `usb`, `lord-server` opens the hidraw device and gives it straight to `lord-vault` (`vc_fido_attach`); the app never holds the device. Only the app spawned with a vault channel may ask. |
| Clipboard | `BOP_CLIP_SECRET`: the copy is marked sensitive (out of the history, password-manager hint for Wayland clients) and `lord-server` clears it after the chosen seconds if it still holds the same text (compared by SHA-256; the text is not kept). |
| Protection | The manifest says `"protected": true`; `sdk/wayland.js` marks the app's Wayland window protected as soon as it maps, so it is black in screenshots, recordings and the remote desktop (`passwords-app-screenshot.png`). |
| Window | Opening Passwords (Dock, app launcher, Search, `wm.open('passwords')`) starts the process or focuses its window; the manifest's `size`/`minSize` now size the app window. |

**Fallback.** If `lord-server` has no app mode or the system package is missing (a dev tree
without `npm run build`), the same UI runs in the shell's context as before (path-based files, the
in-app file picker, the shell clipboard timer). The vault itself is unchanged either way.

Hidden in app mode: "Print" for the recovery key (the print dialog lives in the shell; saving it
to a file works) and the "Open Settings" link on VPN items.

| App mode: the system file dialog over the lock screen | App mode: a LordOS screenshot of the app window |
|---|---|
| ![Powerbox](passwords-app-file-picker.png) | ![Protected app window](passwords-app-screenshot.png) |

In the in-process fallback the lock and create screens show their file picker inline, under the
padlock and the title (`passwords-inline-picker.png`), instead of a centred dialog that
covered the whole form.

## The vault format (`.lvault`, version 1)

One file, portable: copy it, back it up, carry it on a USB stick. The default vault is
`~/.local/share/lordos/vault/contrasenas.lvault`; the session keyring is `login.lvault` next to
it. Both folders are on the `lord.fs` deny list (`fsops_protected`): JavaScript cannot overwrite
or delete them; only `lord-vault` writes there.

```
header   magic "LORDVLT\x1a" | u16 version=1 | u16 minor | u32 header_len | vault_id[16]
         | u64 created | u64 saved | u32 save_counter | u16 flags | u16 nslots
         | slots…: u8 type | u8 kdf | u16 len | id[8] | u64 created | label | kdf params | salt[16]
                   | (FIDO2: credential id, hmac salt[32], aaguid[16], pin flag)
                   | nonce[24] | wrapped_vault_key[32] | mac[16]
body     nonce[24] | u64 len | XChaCha20-Poly1305(JSON content) | mac[16]     AD = whole header
attach.  u32 count | per item: id[16] | nonce[24] | u64 len | ciphertext | mac[16]
trailer  "LVLTEND\0" | u64 total_len
```

- **Vault key (VK):** 32 random bytes. It is never derived from the password. Each *slot* wraps VK
  with XChaCha20-Poly1305 under a key derived from one way of unlocking; the wrap's associated data
  binds the slot to the vault id and to its own parameters.
- **Key derivation per slot:**

  | Slot | KEK |
  |---|---|
  | password | Argon2id(password, salt, m, t, p=1) |
  | key file | BLAKE2b(keyed by BLAKE2b(file), "keyfile" ‖ salt) |
  | password + key file | BLAKE2b(keyed by Argon2id(password), "passkey" ‖ BLAKE2b(file)) |
  | FIDO2 | BLAKE2b(keyed by hmac-secret output, "fido2" ‖ salt) |
  | recovery key | BLAKE2b(keyed by the 200-bit recovery code, "recovery" ‖ salt) |
  | session (keyring) | Argon2id(login password, salt, …) |

- **Body:** subkey `BLAKE2b(VK, "body")`; associated data = the whole header, so changing a slot,
  a KDF parameter or the counter is detected when the body fails to authenticate.
- **Attachments** (licence files, document photos, certificates) are sealed separately with
  `BLAKE2b(VK, "attachment")`, AD = vault id ‖ attachment id; their nonce and MAC are also recorded
  in the (authenticated) body, so swapping or removing an attachment is detected. Unlocking decrypts
  only the body; attachments are decrypted on demand and never all at once.
- **Content (JSON):** `{"v":1,"meta":{…},"cats":[…],"tags":[…],"entries":[{id, cat, title, notes,
  fav, arch, trash, created, modified, pwch, rotate, tags, f:[{k,t,l,v}], att:[…], hist:[…], x}]}`.
  Field types: text, hidden, url, date, number, file, totp, email, multiline, pin, bool. The
  history keeps the last 20 versions of an item's fields (hidden values included, shown only on
  explicit reveal).
- **Atomic save with backup:** write `.name.tmp-PID` (O_EXCL) and `fsync`; hard-link the current
  file to a temporary name and `renameat` it over `name.bak` (so `.bak` is always the previous
  version, replaced atomically); `renameat` the new file over `name`; `fsync` the folder. At every
  moment there is a complete vault on disk. This works inside Landlock because everything happens
  within the one folder `lord-vault` received as an fd.
- **Argon2id cost** is stored per slot and tuned per machine (`vf_calibrate`): it measures one
  pass at 64 MiB, then picks memory (19–256 MiB, at most 1/8 of RAM) and passes for ~0.7 s.
  "Settings → Preferences → Measure" shows what this machine would use; changing the password
  re-measures.
- **Recovery key:** 40 Crockford base32 characters (8 groups of 5, 200 bits). It is shown once at
  creation, can be printed (system print dialog) or saved as a text file, and can be regenerated.
  Typing is forgiving: case, dashes, O/0 and I/L/1 are normalised.

## Unlocking

1. **Master password** (Argon2id).
2. **FIDO2 security key** (YubiKey, SoloKey, Nitrokey, …) with the CTAP2 `hmac-secret` extension:
   own client (`src/vault/ctap2.c`: CTAPHID framing, CBOR, getInfo, clientPIN protocol 1 with our
   own P-256 ECDH, makeCredential and getAssertion with hmac-secret). Enrolling asks for two
   touches (create the credential, then derive). If the key has a PIN, the slot remembers that a
   PIN was used (CTAP 2.1 keys return different secrets with and without user verification).
3. **Key file**: 64 random bytes written to a path the user picks, typically a USB stick under
   `/media/$USER`; any existing file of at least 32 bytes also works (hashed with BLAKE2b).
4. **Password + key file** (two factors). Adding it removes the password-only slot (the UI warns).

Several methods can coexist; at least one besides the recovery key must remain.

**Device access.** `lord.vault.fidoAttach(app)` only works when the app holds the `usb` permission
(system prompt, stored in `permisos.json`, checked in C). `lord-server` finds FIDO devices by the
HID report descriptor (usage page 0xF1D0, `src/vault/fidodev.c`), opens `/dev/hidrawN` and passes
the fd to `lord-vault`. Neither the app nor its JavaScript ever sees the device.
FIDO hidraw nodes need a udev rule because LordOS sessions are not logind seat sessions
(`TAG+="uaccess"` does not apply):

```
# /usr/lib/udev/rules.d/71-lordos-fido.rules
ACTION=="remove", GOTO="lordos_fido_end"
SUBSYSTEM=="hidraw", ENV{ID_FIDO_TOKEN}=="1", GROUP="plugdev", MODE="0660"
LABEL="lordos_fido_end"
```

(`ID_FIDO_TOKEN` comes from systemd's `60-fido-id.rules`; the user must be in `plugdev`.)

**Auto-lock.** The vault locks after the idle timeout (default 5 min, Settings → Preferences),
when the screen is locked (❖L, ❖Ctrl+Q: `mirror_session("lock")` → `vc_lock_all`) and when
switching users. The session keyring stays open until logout, like the login keyring of other desktop systems.
Locking frees the model block by block; every arena block is wiped on free.

## Memory protection (`src/vault/smem.c`)

- Decrypted data lives in our own arena: anonymous `mmap` regions with `mlock` (as far as
  `RLIMIT_MEMLOCK` allows: 8 MiB by default on Debian), `MADV_DONTDUMP` and `MADV_WIPEONFORK`.
  Small blocks come from size classes (O(1)), large ones get their own mapping.
- `sm_free` wipes before releasing; locking a vault frees its whole model.
- The process is not dumpable (`PR_SET_DUMPABLE 0`, which also blocks ptrace from the same user)
  and `RLIMIT_CORE` is 0.
- The request JSON is received straight into the arena (`vw_feed` with the arena allocator) and
  the in-arena JSON reader (`vjson.c`) tokenises in place; stb_image (for QR images and photo
  previews) and the KDBX gzip inflate also allocate from the arena (`vstb.c`).
- Argon2's work area (64–256 MiB) cannot be locked under the default limit; it is `DONTDUMP`
  and wiped after each derivation. The test VM has no swap.

## Clipboard

- Copying a password, PIN, card number or 2FA code calls `clipboard.set(value, { secure: true })`:
  it never enters LordOS' clipboard history (Search / ❖V panel).
- Wayland clients now see the extra MIME type `x-kde-passwordManagerHint` (value `secret`) on a
  sensitive LordOS clipboard, the KDE/GNOME convention clipboard managers honour
  (`src/wl/wl.c`). The bytes handed to Wayland clients are wiped after sending.
- The clipboard is cleared after 30 s (configurable: 15/30/60/120 s) if it still holds the same
  value: by `lord-server` in app mode (`BOP_CLIP_SECRET`), by the app in the in-process fallback.
  Replacing the clipboard also wipes the previous text in `lord-server` (`term_set_clipboard`).

## The session keyring (`login.lvault`)

A separate vault, like the *login* keyring of other desktop systems:

1. `lord-sessiond` (root) verifies the password typed at the login screen (`crypt_r` against
   `/etc/shadow`, unchanged).
2. Right after, the login worker sends it once over the session socket as `RP_KEYRING` (accepted
   only from a root peer, `src/remote/mirror.c`) and wipes its copy.
3. `lord-server` forwards it to `lord-vault` (`session-password` on the control channel) and wipes
   its buffers; `lord-vault` opens `login.lvault` (a `session` slot, Argon2id tuned to ~0.4 s) or
   creates it on first login.
4. If the account password was changed outside LordOS, the slot no longer opens: `lord-vault` emits
   `keyring-mismatch` (the UI can ask for the old password; re-wrapping is a `password-change` on
   `v:"login"`).

When the session starts without a password (autologin, remote desktop for now), the keyring stays
locked; the first app that needs it triggers a system prompt (`<KeyringPrompt />`, mounted by
`startShell`) asking for the login password. When several apps ask at once (0.8.5, `sdk/vault.js`),
the requests queue behind one box: one answer goes to all of them, a new request no longer wipes the
field, and Cancel answers every queued request instead of bringing the box back for the next one.

### `lord.keyring` / SDK `keyring` (native apps)

```js
import { keyring, AutofillButton } from '@lordos/sdk';
await keyring.set('mail', { service: 'imap.example.com', account: 'ana', secret: 'xxxx', label: 'Mail' });
const item = await keyring.get('mail', { service: 'imap.example.com', account: 'ana' }); // { secret, … } | null
await keyring.remove('mail', { service, account });
await keyring.list('mail');                         // without secrets
```

- Items are entries of `login.lvault` with `x.kr = {owner, service, account, shared}`.
- A **system package** (one shipped in `assets/sysapps`, such as Mail since 0.8.2) keeps the owner
  it had when it ran in the session: its plain id (`mail`), not `app.mail`, so Mail's account
  passwords stay where the session's need-auth handler (`sdk/mail.js`) reads them. A user `.lpk`
  can never shadow a system package (`app_package_dir`), so it can never get that owner.
- An app sees only its own items. Asking for another app's item returns `not-shared` plus the item
  id, so the system can ask the user; `keyring.share(id, app)` records the user's decision
  (`shared`). Sharing is an admin operation (the Passwords app or a system prompt), never the
  asking app itself.
- **Trust boundary:** built-in apps (other than the isolated system packages: Passwords, and since
  0.8.2 Mail, Calendar, Contacts and Reminders) run in the shell's JavaScript context, so for them the owner
  string is passed by trusted system code, not enforced by a process boundary. For isolated `.lpk`
  apps the owner is set by `lord-server` from the broker (see below). The Passwords app process
  manages the keyring over its own vault channel, which only the system `passwords` package
  receives.
- **Settings → Network** now stores Wi-Fi passwords (`wifi:<ssid>`) and VPN credentials
  (`vpn:<name>`) in the keyring when you join or save, and offers them back with the 🔑 button.
  The root-owned copy that `wpa_supplicant`/the VPN daemons need to connect at boot is still
  written by the closed `admin.c` actions (as NetworkManager does with system connections).

### Keyring for isolated .lpk apps

An isolated app (`lord-server --app <id>`) has no `lord.vault`; the same SDK calls go through the
broker instead, and the `app` argument is ignored:

```js
import { keyring } from '@lordos/sdk';
await keyring.set(lord.appId, { service: 'sync.example.org', account: 'ana', secret: token, label: 'Sync' });
const item = await keyring.get(lord.appId, { service: 'sync.example.org', account: 'ana' }); // { secret, … } | null
await keyring.remove(lord.appId, { service: 'sync.example.org', account: 'ana' });
await keyring.list(lord.appId);                     // own items and items shared with it, no secrets
```

How it works (`sec/brokerkr.c`, `ui/js_vault.c`):

- The app calls `lord.keyring.request(op, service, account, label, secret)`, a broker request
  (`BOP_KEYRING`, payload `op\0service\0account\0label\0secret`; op is `get`, `set`, `delete`
  or `list`). Service, account and label are at most 255 bytes, the secret at most 64 KiB.
- `lord-server` builds the `kr-*` request itself with **owner = `app.<id>`**, where `<id>` is the
  app id it verified when it spawned the process. The app never sends an owner, and the `app.`
  prefix keeps a package called `mail` away from the desktop's own `mail` items.
- The request goes to `lord-vault` on lord-server's control-side channel as a *private request*
  (`vc_request_private`): its reply goes to a queue that only lord-server's C code reads, never
  to the desktop's JavaScript. lord-server does not use `vc_attach_app` for these apps, because an
  attached channel is the Passwords app's and may manage every item. Nothing waits: the reply
  wakes the main loop (an eventfd) and is sent back to the app; buffers that held a secret are
  wiped.
- An app sees only its own items. If it asks (`get`) for an item another app saved, lord-vault
  answers `not-shared` and lord-server shows the **system consent prompt** («"Notes" wants to use a
  password that "Sync" saved in the keyring», `privacy.ask.keyring`). If the user allows it,
  lord-server records the share for that one item (`kr-share`) and asks again; if not, the app
  gets `keyring.notShared`, and lord-server does not ask again for that app and item until the
  session ends (an app cannot keep the prompt on screen by asking in a loop). A shared item can be read, never changed or deleted, by the other
  app; for `set`, `delete` and `list` another app's items simply do not exist.
- Errors are code lines (docs/LANGUAGES.md): `keyring.locked`, `keyring.noKeyring`,
  `keyring.notFound`, `keyring.notShared`, `keyring.badRequest`, `keyring.saveFailed`,
  `keyring.busy`, `keyring.failed`, `vault.unavailable`. The SDK keeps the short codes of the
  shell (`e.code` = `locked`, `not-found`…, `e.errorCode` = the full code), so the same app code
  works in both places.
- Test: `make -C server build/brokerkr-e2e && server/build/brokerkr-e2e` (needs `build/lord-vault`;
  isolation between two app ids, the prompt denied (and not asked again) and allowed, a desktop owner a package cannot
  reach, a missing item, bad requests, a locked keyring and an app that closed mid-request).

### Autofill in native apps

`<AutofillButton app service|url|ssid onFill />` is a small 🔑 button next to a login field. It
lists what the app saved in the keyring for that service plus matching Passwords items (by host,
or by SSID for Wi-Fi) while the vault is unlocked; the secret leaves `lord-vault` only when the
user picks one. Used in Settings → Network (Wi-Fi password, VPN credentials).

### Secret Service (`org.freedesktop.secrets`)

Our own minimal D-Bus code (`src/vault/dbus.c`, `secretsvc.c`). Before writing it we checked: no
LordOS binary links libdbus or libsystemd (sd-bus), so adding either would have been a new
dependency; the protocol subset needed (SASL EXTERNAL, Hello, RequestName, method calls and
returns, signals, properties) is small. `lord-server` connects the session bus socket
(`DBUS_SESSION_BUS_ADDRESS` or `$XDG_RUNTIME_DIR/bus`) and passes it already connected; the
sandboxed `lord-vault` authenticates and serves on it.

- Objects: `/org/freedesktop/secrets` (Service), `/org/freedesktop/secrets/collection/login`
  (alias `default`), items below it, sessions and prompts.
- Session algorithms: `plain` and `dh-ietf1024-sha256-aes128-cbc-pkcs7` (1024-bit MODP group 2,
  HKDF-SHA256, AES-128-CBC). `secret-tool` (libsecret 0.21.7) negotiates the DH one.
- Items are entries of `login.lvault` with `x.ss = {attrs, ctype}`; non-UTF-8 secrets are stored
  base64.
- If another service already owns the name (gnome-keyring, kwallet), LordOS does not take it.
  Two things keep it LordOS's (0.8.5): `lord-sessiond` starts a session only once the user's session
  bus exists (with automatic login it used to start a few milliseconds earlier, and `lord-vault` then
  never got on the bus), and the lordos package ships
  `/usr/lib/systemd/user/gnome-keyring-daemon.service.d/lordos.conf`, which starts gnome-keyring (when
  an app pulls it in) with only its PKCS#11 component. Its D-Bus activation file runs only when
  nobody owns the name.
- **Access control is the freedesktop one:** like gnome-keyring, any app of the session that can
  reach the bus can read the unlocked collection. Isolated `.lpk` apps cannot open the bus socket.
- **Chromium:** current Chromium releases keep their saved logins in their own database and store
  only the encryption key ("Chromium Safe Storage", schema `chrome_libsecret_os_crypt_password_v2`)
  in the Secret Service. We verified the whole round trip: save a login, restart Chromium, it
  fetches the key from our service and autofills. LordOS does not set `XDG_CURRENT_DESKTOP` to
  GNOME, so Chromium needs `--password-store=gnome-libsecret` to pick libsecret; without it, it
  uses its "basic" store.
- Not implemented: delete prompts, more than one collection (`CreateCollection` returns `login`),
  per-app ACLs, searching a *locked* collection (returns nothing instead of the locked list).

## Import and export (`src/vault/vimport.c`, `kdbx.c`, `csvp.c`, `xmlmini.c`)

All parsers run inside `lord-vault` on bytes read from an fd, allocate from the arena and were
fuzzed (truncation at every offset and random byte flips under ASan/UBSan).

| Format | How |
|---|---|
| KeePass / KeePassXC **KDBX 4.0/4.1** | own reader: Argon2d/Argon2id (Monocypher), AES-KDF, AES-256-CBC or ChaCha20, header SHA-256 + HMAC, HMAC-SHA256 block stream, gzip, inner header and binaries, ChaCha20 protected values, KeePass XML (groups → category tree, tags, times, custom and protected fields, attachments, TOTP in `otp`/legacy keys) |
| KeePass **KDBX 3.1** | same reader (Salsa20 inner stream, hashed blocks); AES-KDF uses AES-NI when present |
| KeePassXC CSV | supported as a fallback; it loses tags, custom fields and attachments |
| Bitwarden JSON / CSV | logins, notes, cards, identities, custom fields, folders; encrypted exports are refused with a clear message |
| Chrome/Chromium, Firefox, CSV from other password managers (Title, URL, Username, Password, Notes, OTPAuth) | yes |
| Passwords CSV / copy (`.lvault`) | export and import; a `.lvault` copy opens with its own unlock methods and is merged |

Twofish KDBX files and Argon2 version 0x10 are not supported. Export to CSV shows a red warning
and needs an explicit confirmation; export to `.lvault` writes an encrypted copy with the same
slots.

## Templates and categories

Built-in categories, each with a field template (`apps/passwords/templates.js`): General,
Internet, Email, Finance, Cards, Network, Wi-Fi (with QR), Servers/SSH, Databases, Apps,
Licenses (attachments), API and tokens, Identity and documents (photo attachments), Crypto (extra
warning), VPN (link to Settings → Network), Secure notes (markdown + attachments). Users create
categories with a name, icon, colour, parent (any depth: a tree) and their own field
definitions; folders of imported password databases and exports become user categories. Icons are
drawn glyphs (`apps/passwords/glyphs.jsx`, no color emoji): a category stores the glyph's name, and
categories saved by older versions (an emoji) are shown with the nearest glyph. Items have one category,
many tags (with colours), favourite, archived, trash, history, attachments and an optional
"change every N days" reminder.

## Security report and generator

- **Weak:** strength below "fair" or in the bundled list of the 10,000 most common passwords
  (SecLists, MIT), also catching case, trailing digits/symbols and leetspeak variants.
- **Reused:** grouped by a keyed BLAKE2b of the password (computed in `lord-vault`).
- **Old:** past their reminder, or unchanged for a year.
- **Breached, offline only:** the common-password list, and optionally a local Have I Been Pwned
  "pwned-passwords-sha1-ordered-by-hash" file chosen by the user (binary search with `pread`).
  Nothing is ever looked up online; `lord-vault` has no network at all.
- **Generator:** passwords (length 8–64, classes, no look-alikes) and passphrases from the EFF large
  word list (7,776 words, CC BY 3.0 US), with separator, capitals and a digit. Random bytes from
  `getrandom` with rejection sampling.

## TOTP and QR codes

- RFC 6238 TOTP and RFC 4226 HOTP with SHA-1/256/512, 6–10 digits, any period; `otpauth://` URIs
  and bare base32 secrets.
- Adding 2FA from a QR image: `qr-scan` decodes the PNG/JPEG inside `lord-vault` with our own QR
  decoder (`src/vault/qr.c`: finder patterns, affine sampling with the alignment pattern, format
  and version info, Reed-Solomon correction).

## Measurements (VM 116: Xeon Silver 4108 @ 1.8 GHz, 4 vCPU, no GPU)

| What | Result |
|---|---|
| Argon2id calibrated for this VM | 128 MiB, 5 passes |
| Unlock with the master password | 435–660 ms (target 0.5–1 s) |
| Session keyring at login | ~0.4 s, created on first login |
| Search 10,001 items (model) | 0.44–1.60 ms |
| Search 10,004 items (inside lord-vault, measured there) | worst 1.53 ms (< 10 ms) |
| List of 10,004 items to the UI | 1.7 MB in 43 ms round trip |
| Open a vault with 10,004 items (recovery key, no Argon2) | 169 ms |
| Import a 10,000-row Chrome CSV | 223 ms total (parse 14 ms) |
| KDBX 4 Argon2d 64 MiB × 10, 300 entries | 579 ms |
| KDBX 3.1 from keepassxc-cli (33.7 M AES-KDF rounds) | 958 ms with AES-NI |
| FIDO2 hmac-secret with the software key | ~307 ms (three simulated 100 ms touches) |
| Wi-Fi QR encode / decode a 1000×1000 screenshot | 0.53 ms / 11.4 ms |
| Secret Service DH modexp (1024 bit) | 5.3 ms |
| lord-vault memory with the demo vault open | 28 KB in use, 256 KB mlocked, all regions locked |

## Testing

`make -C server test` builds and runs, among the rest:

| Test | What |
|---|---|
| `build/monocypher-test` | Monocypher's official test vectors (`tests/test.c` + `vectors.h` of 4.0.2) |
| `build/vcrypto-test` (27) | AES (FIPS-197, SP 800-38A CBC), SHA-1, HMAC (RFC 2202/4231), HKDF (RFC 5869), the arena |
| `build/vault-test` (74) | vault round trip, every unlock method, wrong password, **every byte flipped** (1165/1165 detected), **truncated at every length**, unknown version, atomic save + `.bak`, model upsert/keep/history, 10k-item search < 10 ms, RFC 6238/4226 vectors, otpauth parsing, generator and strength, JSON, chunked messages with fds, Argon2 timing |
| `build/ctap-test` (135) | CBOR (RFC 8949 Appendix A), 200,000 random mutations, P-256 vectors from python-cryptography, PIN protocol math, CTAPHID reassembly |
| `build/dbus-test` (103) | D-Bus marshalling, 316,000 mutated messages, DH vectors, an end-to-end Secret Service over a fake bus |
| `build/import-test` (150) | all formats, 5 real KDBX files, key files, tampering, truncation, fuzzing under ASan/UBSan |
| `build/qr-test` (36) | known-answer QR matrices, 360 round trips, error correction, real PNGs, fuzzing |

Not part of `make test` because they need the real helper or root:

- `build/vault-e2e` (51/51): drives the real sandboxed `lord-vault` through `vclient.c` (create,
  unlock, wrong password, items, search, report, TOTP, QR, attachments, key file on a "USB",
  recovery key, password + key file, import/export, keyring ownership and sharing, screen lock
  closing Passwords but not the keyring, `NoNewPrivs`/`Seccomp` set, and the app channel from
  `vc_attach_app()`: requests over its own socket and the lock event reaching it).
- App mode by hand on VM 116: the app process unlocks, reveals, copies (pasted in the Terminal,
  empty again after 30 s), attaches a file through the powerbox, locks, and its window is black in
  a LordOS screenshot.
- `tests/fido-soft/run-pw-116.sh` (32/32): `softfido`, a software CTAP2 authenticator on
  `/dev/uhid` (CTAPHID, getInfo, clientPIN, makeCredential, getAssertion with hmac-secret), and the
  client against it: same salt → same secret, other salt or credential → different, wrong key →
  `no-credential`, PIN required/invalid/blocked paths, timeout and cancel.
- `tests/secretsvc/run-pw-116.sh` (15/15): a demo backend on the real session bus with
  `secret-tool`.

**Untested: real security keys.** No YubiKey, SoloKey or Nitrokey was available; the FIDO2 path was
exercised only against our software authenticator over uhid (in the tests and in the real session
for the screenshots) and against recorded/synthesised frames. CTAP 2.1 PIN protocol 2 and keys
whose only verification is built in (fingerprint) are not implemented/tested.

## Files

| Path | What |
|---|---|
| `third_party/monocypher/` | Monocypher 4.0.2 (CC0/BSD-2), official tests and vectors |
| `third_party/wordlists/` | EFF large word list (CC BY 3.0 US), 10k common passwords (SecLists, MIT) |
| `server/src/vault/vault.c`, `vault_int.h` | `lord-vault`: process, sandbox, channels, requests |
| `server/src/vault/keyring.c` | session keyring, `kr-*` requests, Secret Service backend |
| `server/src/vault/vfile.c` | the `.lvault` format, slots, KDF, atomic save |
| `server/src/vault/vmodel.c` | decrypted content: items, search, list, history |
| `server/src/vault/smem.c` | the secret arena |
| `server/src/vault/vjson.c`, `vwire.c` | in-arena JSON, the message protocol |
| `server/src/vault/vgen.c`, `totp.c`, `qr.c`, `vstb.c` | generator/strength/HIBP, TOTP, QR, stb with the arena |
| `server/src/vault/vcrypto.c` | AES, SHA-1, HMAC, HKDF |
| `server/src/vault/ctap2.c`, `cbor.c`, `p256.c`, `fidodev.c` | FIDO2 client, CBOR, P-256, device discovery |
| `server/src/vault/dbus.c`, `secretsvc.c` | own D-Bus and the Secret Service |
| `server/src/vault/vimport.c`, `kdbx.c`, `csvp.c`, `xmlmini.c` | importers |
| `server/src/vault/vclient.c`, `server/src/ui/js_vault.c` | lord-server side, `lord.vault` (shell and app mode) |
| `server/src/app/appspawn.c`, `appclient.c` | system package lookup, the vault channel for the app, window size from the manifest |
| `server/src/sec/brokersrv.c`, `server/src/ui/js_broker.c` | `BOP_VAULT_FIDO`, `BOP_CLIP_SECRET` |
| `assets/sysapps/passwords/` | the built app process bundle (generated by `npm run build`) |
| `server/mk/vault*.mk` | build and test rules |
| `sdk/vault.js` | `vaultRequest`, `keyring`, `autofill`, `<AutofillButton>`, `<KeyringPrompt>` |
| `apps/passwords/` | the app |
| `assets/locales/{en,es,pt}/passwords.json` | texts |
| `tests/fido-soft/` | software FIDO2 authenticator |
