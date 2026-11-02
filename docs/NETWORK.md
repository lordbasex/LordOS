# Network and VPN in LordOS

LordOS's networking is its own and runs directly on top of the kernel. It doesn't use NetworkManager, D-Bus, libnl, wg-quick or wireguard-tools. The code is in `server/src/net/` and has two sides:

| Side | What it does | Files |
|---|---|---|
| lord-server (without root) | Reads the state with rtnetlink (interfaces, addresses, routes, counters) and listens for changes. DNS comes from `/run/systemd/resolve/resolv.conf` or `/etc/resolv.conf`. It exposes it to JS as `lord.net`. | `state.c`, `ui/js_net.c` |
| `lord-netd` (root, the system network) | Serves the `net-*` actions of `admin.c`'s closed list and owns the resident processes (DHCP, wpa_supplicant, VPN). `lord-sessiond` starts and supervises it at boot (see "The network belongs to the system"). Without `lord-sessiond`, the session's helper serves them, as before. | `actions.c`, `dhcp.c`, `wifi.c`, `wg.c`, `ovpn.c`, `ipsec.c` + `vici.c`, `vpnproc.c` (PPTP, OpenConnect, Fortinet), `vpn.c` (the common parts), `sys.c` |
| shared | Our own netlink, minimal cryptography (X25519, SHA-1/PBKDF2, base64), utilities | `nl.c`, `crypto.c`, `netpriv.h` |

## The network belongs to the system: `lord-netd` (M9)

Up to M8 the network was brought up by the root helper of the **first session** that logged in
(`/run/lordos/net-owner`). On an installed system that had three problems:

- there was no network before the first login, so the greeter showed the hostname instead of the
  IP and the remote desktop was unreachable;
- if that session logged out, its DHCP client and wpa_supplicant exited with it, and the other
  sessions didn't bring the network back;
- the network depended on who logged in first.

Now the network belongs to the system, like NetworkManager or systemd-networkd:

```
lordos.service
└── lord-sessiond
    └── lord-netd                       the network: DHCP, saved Wi-Fi, configs, DNS, VPN
        ├── if:<if>                     DHCP client (our own code)
        ├── wpa:<if>                    wpa_supplicant
        └── vpn:<name>                  openvpn, charon, pppd…
lord-server --user lord ── lord-admin ──┐  /run/lordos/netd.sock (0600, root only)
lord-server --user ana  ── lord-admin ──┘  net-* actions are forwarded as they are
```

- **What it is.** `lord-netd` is `admin.c` built with `LORD_NETD` (`server/src/net/netd.c` only has
  `main`). Its table has **only** the `net-*` actions, with the same validation, and it runs
  nothing those actions didn't already run. libc only.
- **Its own cgroup leaf** (0.8.5): before its execve, the child moves itself into the root-owned
  `lordos.service/netd` leaf (`cgroup_service_leaf`), next to lord-sessiond's `supervisor` leaf, so
  Monitor → Network can count the network service's own traffic ("Network service", never
  blockable; `FIREWALL.md`, "The essential system services"). A lord-netd adopted from before the
  update stays where it was until it restarts.
- **When it starts.** `lord-sessiond` starts it at boot, before the greeter
  (`server/src/remote/netsup.c`):
  - fixed path `<lord-sessiond's directory>/lord-netd`, root-owned and not writable by others
    (unless `insecure_dev`);
  - no arguments and a minimal environment;
  - in its own session (`setsid`).

  `lord-netd` runs `net-restore` by itself: the configs in `/etc/lordos/net/if`, DHCP on free
  physical NICs, Wi-Fi with the saved networks, and DNS. That is why there is an IP before anyone
  logs in.
- **Sessions** bring nothing up. Each session's helper connects to `/run/lordos/netd.sock`
  (`SO_PEERCRED`: uid 0 only), introduces itself with `HELLO <user>` and forwards every `net-*`
  action as it is. `lord-netd` works out again whether that user is an administrator (groups sudo, admin or wheel)
  and applies `A_ANYONE` as before. Replies go back to the helper that asked; events
  (`net dhcp…`, `net wifi…`, `net vpn…`) go to every session. Settings → Network didn't change.
- **A session's `net-restore`** answers `OK net.restoreSystem` ("The network is managed by the system").
- **Wi-Fi and VPN passwords** stay in `/etc/lordos/net/wifi` and `/etc/lordos/net/vpn` (root, 0600).
  They travel over the socket between two root processes and never go back to a session.
- **If `lord-netd` exits,** `lord-sessiond` starts it again: it waits 1, 2, 4… up to 30 s, and goes
  back to 1 s if it had run for more than a minute.
  - Its DHCP client gets `SIGUSR1` (`PR_SET_PDEATHSIG`): it exits **without releasing the address**
    and leaves the lease in `/run/lordos/dhcp/<if>.lease` (0600). The new client confirms it
    (INIT-REBOOT) and connections are not dropped.
  - VPNs that were up are reconnected (`/run/lordos/netd-vpns`).
  - wpa_supplicant does restart: Wi-Fi reassociates within a few seconds.
- **If `lord-sessiond` restarts** (`KillMode=process`), `lord-netd` keeps running and the new
  `lord-sessiond` **adopts** it: it reads `/run/lordos/netd.pid`, checks that it is a root process
  named `lord-netd` and watches it with a `pidfd` in its `poll`, without polling. The lock
  `/run/lordos/netd.lock` (`flock`) prevents two from running.
- **Sessions reconnect:** when `lord-netd` restarts, each session's helper sees it come back with
  inotify on `/run/lordos` (the socket is created under another name and renamed once it listens)
  and reconnects. An action in progress answers `ERR netd.restarted` ("The system network service
  restarted; try again").
- **Without `lord-sessiond`** (a standalone `lord-server`, in development) there is no
  `/run/lordos/netd.enabled`, and the session's helper runs the network itself, as before (with
  `net-owner`). With that mark, a session never brings the network up on its own.
- **Resident processes no longer inherit the helper's descriptors** (`admin_daemon_fork` and
  `A_FORK` actions keep only their pipe): a DHCP client that outlives `lord-netd` doesn't hold its
  lock or socket.

### Test (VM 119, October 17)

VM 119 runs `lord-sessiond` installed like the `.deb`; it was not installed from the ISO (see
`VM-SETUP.md`, step 23). `eth0` is managed by systemd-networkd (netplan) and LordOS leaves it
alone, so the test uses a LordOS-managed NIC: the veth `lnt0`, whose peer is in the netns `lnt`
with a `dnsmasq` there (`lordos-test-netns.service`, which starts before `lordos.service`), and
`/etc/lordos/net/if/lnt0.conf` with `method=dhcp`.

| Test | Result |
|---|---|
| Boot, nobody logged in | `lord-netd` started together with the greeter; `lnt0` had `10.77.0.96/24` after **3 s** |
| Remote desktop before login | `curl https://10.77.0.96:5673/` from the netns: **200** |
| `lord` logs in on the monitor, `ana` (created in Settings → Users) over the network | two helpers connected to `netd.sock`; Settings → Network shows `lnt0` "por DHCP" |
| `lord` logs out (it was the "first" session) | `lnt0` keeps its IP |
| `ana` logs out too: no session left | `lnt0` keeps its IP and the remote desktop answers 200 |
| `kill -9` on `lord-netd` | restarted in 1 s, lease `resume`: the address **never dropped** (sampled every 0.5 s) |
| `systemctl restart lordos` | `lord-netd still running (pid …): the network was not interrupted`; killing it afterwards: restarted and resumed the same way |

| | |
|---|---|
| ![greeter](network-login-screen.png) | ![Settings → Network](network-session.png) |
| Greeter right after boot, nobody logged in: the IP at the bottom. On this VM it is `eth0`'s (netplan); `lnt0`'s (lord-netd) was already up, as the log shows | Logged in: `lnt0` "por DHCP", brought up by `lord-netd` |
| ![greeter, no sessions](network-login-screen-no-sessions.png) | |
| Both users logged out: back at the greeter, the network stays up | |

A fix that came out of the test: `systemctl restart lordos` failed with "Failed to spawn executor:
Device or resource busy" while processes were still alive (greeter, sessions and now `lord-netd`):
systemd tries to put the new main process in the service cgroup's root, which has controllers
enabled. `lordos.service` now has `DelegateSubgroup=supervisor`.

## What's there (tested in the VM)

| Feature | How | Status |
|---|---|---|
| Interfaces, addresses, routes, counters, DNS | rtnetlink without root | ✅ |
| Change notifications | rtnetlink groups in lord-server's `poll` → `__lordNet` | ✅ |
| DHCPv4 | Our own client (RFC 2131). Uses AF_PACKET, renews at T1 by unicast and asks again at T2. DHCPRELEASE when stopping. | ✅ against dnsmasq: lease, renewal and release |
| Static IP | address/prefix, gateway and DNS | ✅ |
| DNS | With systemd-resolved: `resolvectl` per interface. Without resolved: `/etc/resolv.conf`. | ✅ resolved |
| Coexistence with systemd-networkd | System interfaces are shown but not touched. `net-manage` hands them to LordOS and `net-unmanage` gives them back. | ✅ with a test interface |
| Coexistence with NetworkManager | Interfaces with `managed=true` in `/run/NetworkManager/devices/<ifindex>` (read without D-Bus) are system interfaces too: lord-netd doesn't start DHCP or wpa_supplicant on them. `net-manage` only knows how to take them from systemd-networkd. | ✅ code; found on a Raspberry Pi OS board (2026-10-18), where lord-netd had started a second DHCP client and wpa_supplicant |
| Wi-Fi WPA2-Personal and open networks | wpa_supplicant (Debian) through its control socket. The PSK is computed with PBKDF2. | ✅ scan, wrong key, connection and DHCP, reconnection on restart, forget (with mac80211_hwsim + hostapd) |
| Wi-Fi WPA3 (SAE) | `sae_password` and `key_mgmt WPA-PSK SAE` | ⚠️ untested: the test hostapd is WPA2 |
| WireGuard | Our own generic netlink (`WG_CMD_SET_DEVICE`/`GET_DEVICE`) and our own X25519 keys | ✅ against the official `wg`: split and full tunnel (0.0.0.0/0 with fwmark and rules like wg-quick), DNS, status |
| OpenVPN | Debian's program, with the config filtered by a list of allowed options and the management interface | ✅ against an openvpn 2.6 server: inline certificates, username and password (including the wrong password), pushed routes and DNS |
| IPsec/IKEv2 (included) | strongSwan: Debian's `charon`, one per VPN, driven over VICI with our own client (`vici.c`, without swanctl). XFRM interface `ips-<n>` with `if_id`. | ✅ against a charon 6.0.1 in a netns: EAP-MSCHAPv2 (correct and wrong password), PSK, virtual IP, pushed DNS, split tunnel (host) and full tunnel (netns) |
| IPsec with client certificate (`auth=cert`) | the same, with `load-key` | ⚠️ implemented, untested |
| PPTP (included, **insecure**) | Debian's `pppd` + `pptp` with the pppd config LordOS writes | ✅ against `pptpd` in a netns: MS-CHAPv2, mandatory MPPE 128 negotiated, routes, DNS, wrong password |
| OpenConnect (optional: `net-vpn-enable openconnect`) | Debian's `openconnect` with our own fixed-content script | ✅ AnyConnect against `ocserv` 1.3 in a netns: certificate pin, split routes and DNS/domain sent by the server, wrong password, unknown certificate without pin (fails correctly). Installation with LordOS's package engine was tested too. GlobalProtect and Pulse: ⚠️ no test server |
| Fortinet (optional: `net-vpn-enable fortinet`) | Debian's `openfortivpn` | ⚠️ **not tested against a FortiGate** (no free server available). Tested: installation, generated config, startup, "Connection refused" error against a closed port, name that doesn't resolve |
| DHCPv6, 802.1X/WPA-Enterprise | — | pending |

## The UI: Settings → Network

It isn't a separate app: it's in Settings, as on popular desktop systems. At the top it has a row with each
connection, Wi-Fi and VPN (`apps/settings/Network.jsx`). Control Center shows the main connection
and whether there are active VPNs, and opens that section.

| | |
|---|---|
| ![network](settings-network.png) | ![vpn](vpn-wireguard.png) |
| eth0, managed by systemd-networkd: LordOS shows it and doesn't touch it, unless it's handed to LordOS on purpose | WireGuard imported from `~/Descargas/oficina.conf` and connected: last handshake and traffic. The peer was in a test netns; ping through the tunnel answered and eth0's DNS didn't change |
| ![cc](control-center-network.png) | |
| Control Center: network and active VPNs | |

## JS API: `lord.net` (read-only, without root)

| Call | Returns |
|---|---|
| `lord.net.interfaces()` | `[{ name, index, type, kind, up, carrier, operstate, mac, mtu, master, managed, rxBytes, txBytes, rxPackets, txPackets, rxErrors, txErrors, addresses: [{ family: 4\|6, address, prefix, scope: 'global'\|'link'\|'host', dynamic, validLifetime }], config }]` |
| `lord.net.routes()` | `[{ family, dst: 'default'\|'a.b.c.d/n', gateway, dev, metric, table, protocol: 'kernel'\|'boot'\|'static'\|'dhcp'\|'ra'\|n }]` (without the local table or the fe80::/64 routes) |
| `lord.net.dns()` | `{ source: 'systemd-resolved'\|'resolv.conf', servers: [...], search: [...] }` |
| `lord.net.config(if)` | `{ method, addresses: [], gateway, dns: [] }` or `null` (what LordOS saved) |
| `lord.net.vpnList()` | No secrets. `[{ name, type: 'wireguard', ifname, server, address, fullTunnel, publicKey, peers }` \| `{ name, type: 'openvpn', ifname, server, needsAuth, hasCredentials }` \| `{ name, type: 'ipsec', ifname, server, auth: 'eap'\|'psk'\|'cert', user, fullTunnel, insecure: false }` \| `{ name, type: 'pptp'\|'openconnect'\|'fortinet', ifname, server, user, fullTunnel, insecure }]` (`insecure: true` only for PPTP) |
| `lord.net.vpnTypes()` | `[{ type, available, optional, package, insecure }]` for the six types: `wireguard`, `openvpn`, `ipsec`, `pptp` (included) and `openconnect`, `fortinet` (`optional: true`: if `available` is `false`, the UI shows "Enable" → `net-vpn-enable`). `package`: the Debian packages needed. |
| `lord.net.wifiKnown()` | `[{ ssid, security: 'psk'\|'open' }]` |
| `lord.net.prepareOvpn(path)` | Text of the `.ovpn` with `ca`/`cert`/`key`/`tls-auth`/`tls-crypt`/`extra-certs` inlined from the same folder. Only accepts files inside `$HOME`; if a file is missing, it throws an error. |
| `globalThis.__lordNet(what)` | Signals that `'link'`, `'addr'` or `'route'` changed. The app asks for the data again. |

Field values:
- `type`: `ethernet`, `wifi`, `wireguard`, `tun`, `bridge`, `loopback`, `other` or the kernel's `kind` (`veth`, `vlan`, `bond`, `dummy`…).
- `managed`:
  - `system`: managed by systemd-networkd or ifupdown. LordOS doesn't touch it.
  - `lordos`: it has a LordOS configuration, or it is a LordOS VPN.
  - `none`: free.
- Counters: `interfaces()` does a fresh dump on every call, so for live traffic it is enough to call it every one or two seconds.

## Actions (`admin.run('net-…', arg)`)

The argument can have several lines (`\n`). "Anyone" means you don't need to be an administrator, as in NetworkManager.
Replies are code lines, never prose (`server/src/admin.h`, `errcode.h`): `OK <code> key=value…` or
`ERR <code> …`, which the UI translates with `t('status.<code>')` and `t('errors.<code>')`; a few
replies carry only data (a count, a public key).

| Action | Argument | Who | Response |
|---|---|---|---|
| `net-restore` | — | anyone | With `lord-netd`, it runs it by itself at boot; the one each session sends answers `OK net.restoreSystem`. Without `lord-netd` (development) the first session applies it, noted in `/run/lordos/net-owner` while alive. It applies the configs in `/etc/lordos/net/if`, and free physical interfaces without a config go through DHCP (with Wi-Fi: wpa_supplicant plus the saved networks). `OK net.restored count=<n>` |
| `net-up` / `net-down` | `if` | admin | `net-down` also stops its DHCP/wpa |
| `net-config` | `if\nmethod=dhcp\|static\|off\naddress=ip/n\n…gateway=ip\ndns=ip…` | admin | Saves and applies. Rejects system interfaces. |
| `net-manage` / `net-unmanage` | `if` | admin | Writes and deletes `/etc/systemd/network/00-lordos-<if>.network` (`Unmanaged=yes`) and runs `networkctl reload`. |
| `net-wifi-scan` | `if` | anyone | One `LOG` per network with `{ ssid, bssid, freq, signal, security: open\|wep\|wpa\|wpa2\|wpa2/wpa3\|wpa3\|enterprise, known }`, then `OK <n>` (≈3–10 s) |
| `net-wifi-connect` | `if\nssid\npassword` (empty if open) | anyone | `OK wifi.connecting ssid=<ssid>`. The result arrives as an EVENT. |
| `net-wifi-disconnect` | `if` | anyone | |
| `net-wifi-forget` | `ssid` | admin | |
| `net-vpn-import` | `type\nname\nconfig`, type `wireguard\|openvpn\|ipsec\|pptp\|openconnect\|fortinet` | admin | WireGuard: `OK <public key>`; with `PrivateKey = generate` it creates it. The others: `OK vpn.imported`. For OpenVPN, first `lord.net.prepareOvpn`. The formats are below. If the program is missing: `ERR vpn.missingProgram type=<type> package=<package>` |
| `net-vpn-enable` | `openconnect\|fortinet` | admin | Installs `openconnect` or `openfortivpn` with LordOS's package engine (the same as `pkg-install`): `LOG progress …` and the package engine's `OK`. If it was already there: `OK vpn.alreadyInstalled`. |
| `net-vpn-credentials` | `name\nuser\npassword` | admin | OpenVPN only. Stored with mode 0600. |
| `net-vpn-up` / `net-vpn-down` | `name` | anyone | WireGuard: `OK vpn.connected`. The others: `OK vpn.connecting` and then an EVENT. |
| `net-vpn-delete` | `name` | admin | |
| `net-vpn-status` | `name` or empty | anyone | One `LOG` per VPN and then `OK <n>`:<br>`{ name, type:'wireguard', ifname, active, peers: [{ publicKey, endpoint, lastHandshake (unix), rxBytes, txBytes }] }`<br>`{ name, type:'openvpn', ifname, running, active }`<br>`{ name, type:'ipsec'\|'pptp'\|'openconnect'\|'fortinet', ifname, running, active, address }` |

VPN names accept `[A-Za-z0-9_-]`, up to 24 characters. The interface, lowercased and trimmed to 15 characters, is `wg-`, `ovpn-`, `ips-`, `ppp-`, `oc-` or `fvpn-` + the name.

For every type other than WireGuard, `net-vpn-up` answers `OK vpn.connecting` and the result arrives as an EVENT. Only one IPsec can be connected at a time, because charon uses `/run/charon.pid`; the second one answers `ERR vpn.ipsecBusy` ("Another IPsec VPN is already connected").

### Import formats (IPsec, PPTP, OpenConnect, Fortinet)

They go in the third field of `net-vpn-import`, with one `key=value` line per row.
- Empty lines and lines starting with `#` are ignored.
- An unknown key rejects the file.
- No value can contain control characters and the maximum length is 255.
- A PEM value (`-----BEGIN …`) continues on the following lines up to its `-----END …-----`.

**`ipsec`** (IKEv2):

| Key | |
|---|---|
| `server` | required: name or IP |
| `remote_id` | server identity. If missing, `server` is used. |
| `auth` | `eap` (username and password with EAP-MSCHAPv2, the typical case for providers), `psk` or `cert` |
| `user`, `password` | for `eap` |
| `psk`, `local_id` | for `psk`: the key has at least 8 characters; `local_id` is optional |
| `cert`, `key` | PEM, for `cert`. `local_id` is optional. |
| `ca` | PEM, optional. If missing, the authorities in `/etc/ssl/certs` apply. |
| `ike`, `esp` | proposals, optional (e.g. `aes256-sha256-modp2048`) |
| `routes` | `10.0.0.0/8,192.168.1.0/24`: split tunnel. If missing, all traffic goes through it. |

It always requests a virtual IP (`vips`).

**`pptp`**: `server` (name or IPv4), `user`, `password`, `mppe` (`required`, which is the default, `optional` or `off`) and `routes` (optional; if missing, all traffic goes through it).

**`openconnect`**:

| Key | |
|---|---|
| `server` | `host[:port][/path]` |
| `protocol` | `anyconnect` (default), `gp`, `pulse` or `fortinet` |
| `user`, `password` | |
| `usergroup` | optional (`--authgroup`) |
| `servercert` | optional: `pin-sha256:…` or the fingerprint in hex |

Routes and DNS are sent by the server. If it sends no routes, all traffic goes through it.

**`fortinet`**: `host`, `port` (443 by default), `user`, `password`, `trusted-cert` (64-character hex sha256 fingerprint; there can be several separated by commas) and `routes` (optional; if missing, all traffic goes through it).

**Full tunnel.** The system's default route is never touched. It's done like OpenVPN's `redirect-gateway def1`:
- `0.0.0.0/1` and `128.0.0.0/1` through the tunnel;
- a `/32` route to the server through where it went before.

All three use protocol 76 (`RTPROT_LORDOS`): they're deleted on disconnect, and `net-restore` deletes any left over from an interruption.

### Notifications (`admin.onEvent`, text)

```
net dhcp <if> started | stopped | bound <ip>/<n> <gw|-> <seconds> | no-carrier | nak | expired | reboot-timeout | error <reason>
net wifi <if> connected <ssid> | disconnected | wrong-key | no-sae | scan-results
net vpn <name> connecting | connected [ip] | reconnecting | disconnected | need-auth | auth-failed | state <state> | error [reason] | log <openvpn error line>
daemon <process> <line>        (wpa_supplicant output: "wpa:<if>")
daemon <process> exit <code>   (end of "if:<if>", "wpa:<if>", "vpn:<name>")
```

## Security

- **Nothing from the user goes to a shell or as a free-form argument.**
  - IPs, prefixes, interface names, SSIDs and keys are validated in `actions.c`.
  - Programs have fixed paths: `/usr/sbin/wpa_supplicant`, `/usr/sbin/openvpn`, `/usr/lib/ipsec/charon`, `/usr/sbin/pppd`, `/usr/sbin/pptp`, `/usr/sbin/openconnect`, `/usr/bin/openfortivpn`, `/usr/bin/resolvectl` and `/usr/bin/networkctl`.
  - No password goes on the command line. They go over VICI (IPsec), in 0600 files (pppd, openfortivpn) or through stdin (openconnect).
- **wpa_supplicant.**
  - We write its config and it only has the control socket.
  - Networks are loaded through that socket: the SSID goes in hex and the WPA2 key as an already computed PSK.
  - The WPA3 password travels in quotes and is rejected if it contains control characters.
- **WireGuard.**
  - `PostUp`/`PreUp`/`PostDown`/`PreDown`/`SaveConfig`/`Table`/`FwMark` are ignored with a warning: LordOS doesn't run commands.
  - An unknown key rejects the file.
- **OpenVPN.**
  - Only the client options in `ovpn.c`'s `allowed` list pass.
  - `up`, `down`, `script-security`, `plugin`, `route-up`, `config`, `cd`, `setenv`, `management`, `log`, `<connection>`, `auth-user-pass file`, certificates as files, etc. reject the file (tested).
  - LordOS adds `script-security 1` (only openvpn's own programs, such as `ip`), its own management interface and `verb 3`.
  - DNS servers pushed by the server are read from the `PUSH_REPLY` and applied by LordOS, because OpenVPN doesn't apply them without scripts.
- **IPsec.**
  - Each VPN has its own `charon` with a generated `strongswan.conf`. The file goes in `/etc/strongswan.d/lordos/<n>.conf` (0600) and the VICI socket in `/run/charon.lordos-<n>.vici`.
  - Those paths are the ones Debian's AppArmor profile allows for charon: with others, charon doesn't start ("invalid configuration"). The system's `strongswan.conf` doesn't include that folder.
  - charon loads only the plugins LordOS names: no `updown` (scripts) or `stroke`.
  - Keys, passwords and certificates travel over VICI with our own client. swanctl isn't used because its AppArmor profile only lets it talk to `/run/charon.vici`.
  - The DNS pushed by the server is taken from charon's log. The `resolve` plugin points to `resolvconf` at `/nonexistent`, so it touches nothing.
  - It uses random ports (`port = 0`), so it doesn't clash with a system strongSwan.
- **PPTP** is insecure: MS-CHAPv2 is broken and MPPE is RC4. It's there because old networks ask for it, and `vpnList`/`vpnTypes` mark it with `insecure: true`.
  - LordOS writes the pppd config. The `pty` is the only command: `/usr/sbin/pptp <already resolved IP> --nolaunchpppd`.
  - `ip-up`/`ip-down` point to a file that doesn't exist, and there is no way to add `plugin`, `connect` or any other option.
  - Only MS-CHAPv2 is accepted: PAP, CHAP, MS-CHAP and EAP are rejected.
- **OpenConnect.**
  - Its `--script` is `/etc/lordos/net/oc-script`, with fixed content. It only records in `/run/lordos/oc/<if>.connect` the variables the server sends (address, routes, DNS), and LordOS validates and applies them. vpnc-script isn't used.
  - The server certificate is validated against the system CAs or the `servercert` (pin) from the import.
- **Fortinet.** openfortivpn runs with `set-routes = 0`, `set-dns = 0` and `pppd-use-peerdns = 0`: LordOS sets routes and DNS. With `set-routes` it would change the system's default route. The pppd that openfortivpn launches does run the system's `/etc/ppp/ip-up.d` scripts; they don't belong to the user.
- **Secrets.**
  - They go in `/etc/lordos/net/vpn/<n>.conf|.ovpn|.auth|.ipsec|.pptp|.openconnect|.fortinet|.<ca|cert|key>.pem` and `/etc/lordos/net/wifi/<ssid-hex>.key`, root 0600.
  - Also in the generated configs: `/etc/lordos/net/ppp/<n>.options` and `/etc/lordos/net/fortinet/<n>.conf`, 0600.
  - The UI only sees the public `.json` files (0644). No action returns a private key; the public one, yes.
- **DNS.** It is only changed on LordOS interfaces (never on `managed: system` ones) and only from the system's network namespace (`sys.c`).
  - Why the namespace matters: `resolvectl` talks to the system's systemd-resolved with the interface number from its own namespace. Launched from another netns, that number may be eth0's.
  - This happened in a test: the tunnel's DNS ended up on eth0 and the VM was left without name resolution until running `resolvectl revert eth0`.
  - When a VPN goes down its DNS is reverted with `resolvectl revert`, while the interface still exists.

## Resident processes

They are children of `lord-netd` (or, without it, of the session's helper), which starts them with `admin_daemon_*`:

| Process | What it is |
|---|---|
| `if:<if>` | DHCP client. On Wi-Fi it also translates wpa_supplicant's notifications. |
| `wpa:<if>` | wpa_supplicant. Leaves a pidfile in `/run/lordos/wpa/<if>.pid` to kill one left over from a previous session. |
| `vpn:<name>` | Drives openvpn, charon, pppd, openconnect or openfortivpn. The program dies with it (`PDEATHSIG`). |

If `lord-netd` restarts, it brings everything back up and resumes the DHCP leases and the VPNs that were up. After a shutdown, VPNs don't reconnect by themselves.

## How to reproduce the test

Nothing touches eth0, where SSH comes in. Everything runs on veth, network namespaces and virtual radios.

```sh
make -C server build/net-test
./build/net-test crypto     # X25519 (RFC 7748), WPA PBKDF2 (IEEE 802.11i), base64
./build/net-test state      # what lord.net sees
```

`sudo net-test act <action> '<arg with \n>'` runs an action like the helper does. `@file` adds a file. Resident processes leave their log in `/run/lordos/test/<process>.log`.

**DHCP**

```sh
sudo ip netns add lnt
sudo ip link add lnt0 type veth peer name lnt1
sudo ip link set lnt1 netns lnt
sudo ip -n lnt addr add 10.77.0.1/24 dev lnt1
sudo ip -n lnt link set lnt1 up
sudo ip netns exec lnt dnsmasq --no-daemon --conf-file=/dev/null --interface=lnt1 --bind-interfaces \
    --dhcp-range=10.77.0.50,10.77.0.99,2m --dhcp-option=6,10.77.0.1 &
sudo ./build/net-test act net-config 'lnt0\nmethod=dhcp'
```

Result: `bound 10.77.0.96/24 10.77.0.1 120`. After 60 s it renews (T1); `method=off` sends DHCPRELEASE.

**Split WireGuard**

```sh
# netns lnw with wg0 (official wg) at 10.88.0.2:51820
sudo ./build/net-test act net-vpn-import 'wireguard\nprueba\n@/tmp/wg.conf'    # PrivateKey = generate
sudo ./build/net-test act net-vpn-up prueba
ping 10.99.0.1
```

`net-vpn-status` shows the handshake.

**Full WireGuard (`AllowedIPs = 0.0.0.0/0`)** is tested inside another netns, `lnc`, because in the system namespace it would cut SSH:

```sh
sudo ip netns exec lnc ./build/net-test act net-vpn-up full
```

There you get rules 32764 (`main suppress_prefixlength 0`) and 32765 (`not fwmark 0xca6c lookup 51820`). Inside a netns DNS is not touched, on purpose.

**OpenVPN**

1. A PKI with `openssl` and an openvpn 2.6 server in `lnw` (UDP 1194, `tls-crypt`, `auth-user-pass-verify`, DNS and route push).
2. The client's `.ovpn` references the files next to it (`ca ca.crt`…).
3. `net-test ovpn-inline` (the same as `lord.net.prepareOvpn`), then `net-vpn-import`, `net-vpn-credentials` and `net-vpn-up`.

**Wi-Fi**

```sh
sudo modprobe mac80211_hwsim radios=2
sudo iw phy phy1 set netns name lnw
# in lnw: hostapd with WPA2 "LordTest" and dnsmasq on wlan1
sudo ./build/net-test act net-restore ''           # wpa_supplicant on wlan0
sudo ./build/net-test act net-wifi-scan wlan0
sudo ./build/net-test act net-wifi-connect 'wlan0\nLordTest\nclave-segura-1'
```

**IPsec**

1. A server `charon` in `lnw`, with a private `/run` (`unshare -m` + tmpfs) and its config in `/etc/strongswan.d/lordtest/` and `/etc/swanctl/lordtest/`, because of AppArmor.
2. The `eap` connections (EAP-MSCHAPv2, server certificate from a test CA) and `psk`, with pool `10.20.0.0/24`, DNS `10.21.0.1` and `local_ts 10.21.0.0/24`.
3. On the client:

```
server=10.88.0.2
auth=eap
user=lord
password=s3creta
routes=10.21.0.0/24
ca=-----BEGIN CERTIFICATE-----
…
```

Then `net-vpn-import` and `net-vpn-up`. Result: `connected 10.20.0.1`, ping to 10.21.0.1 through `ips-<n>` and DNS 10.21.0.1 only on that interface. With the wrong password: `auth-failed`. The full tunnel is tested inside the `lnc` netns.

**PPTP**

1. `pptpd -c … -f` in `lnw`, with `require-mschap-v2` and `require-mppe-128`.
2. The test user goes in `/etc/ppp/chap-secrets` (restored at the end).
3. Result: `connected 10.30.0.10`, and in pppd's log `MPPE 128-bit stateless compression enabled`.

**OpenConnect**

1. `net-vpn-enable openconnect`.
2. `ocserv -f -c …` in `lnw`, with `auth = plain`, the test CA's certificate, `route = 10.21.0.0/24`, `dns` and `default-domain`.
3. Import with `servercert=pin-sha256:…`.
4. Result: `connected 10.40.0.11`, split route, DNS and domain on `oc-<n>`.

**Fortinet**

1. `net-vpn-enable fortinet`.
2. `net-vpn-up` against `10.88.0.2:10443`, a closed port. Result: `error ERROR:  connect: Connection refused` and then `disconnected`.

**To clean up:** `ip netns del …`, `rmmod mac80211_hwsim` and delete `/etc/lordos/net/if`.

## Packages in the VM

| Package | What for |
|---|---|
| `wpasupplicant` | Wi-Fi (LordOS launches it). It ships `wpa_supplicant.service`, which in D-Bus mode doesn't touch interfaces; on a LordOS image it should be disabled. |
| `openvpn` | OpenVPN VPN (LordOS launches it). `openvpn.service` is an empty service. |
| `strongswan-charon`, `libcharon-extauth-plugins` | IPsec/IKEv2: the `charon` daemon and the EAP-MSCHAPv2 plugin. This is the minimum: `charon-systemd` and `strongswan-swanctl` aren't needed. `strongswan-charon` pulls in `strongswan-starter`, which enables itself and was disabled (`systemctl disable --now strongswan-starter`); on a LordOS image it ships disabled. |
| `pptp-linux`, `ppp` | PPTP |
| `openconnect`, `openfortivpn` | **Optional.** Not in the image: `net-vpn-enable` installs them. In the VM they were installed that way and removed again so the UI could test "Enable". |
| `hostapd`, `dnsmasq-base`, `iw`, `wireguard-tools` | Tests only: access point, DHCP server, moving radios between namespaces and the "official" WireGuard peer to compare against. |
| `pptpd`, `ocserv` | Tests only: PPTP and AnyConnect servers. Their services were left disabled (`systemctl disable --now`). |
| `strongswan-swanctl` | Tests only: loads the server charon's config. |

Already there: `openssl` (test PKI), systemd-networkd, systemd-resolved and the `wireguard` and `mac80211_hwsim` modules of kernel 6.12.

## Pending

- **Untested VPNs:**
  - Fortinet against a real FortiGate.
  - GlobalProtect and Pulse/Ivanti with OpenConnect.
  - IPsec with a client certificate.
  - L2TP/IPsec, which isn't there.
- **VPN, more:** several IPsec connections at once, which today isn't possible because of charon's single pidfile. IPv6 inside the PPTP, OpenConnect and Fortinet tunnels (today IPv4 only).
- **Wi-Fi:** test WPA3/SAE with an SAE hostapd; WPA-Enterprise (802.1X); hidden networks from the UI; choosing among several BSSIDs.
- **IPv6:** DHCPv6 (today the kernel does SLAAC on its own) and DNS via RA (RDNSS).
- **VPN at boot:** a per-VPN "connect automatically" option. Today interfaces, Wi-Fi and the VPNs that were up before a `lord-netd` restart are restored, but not those from before a shutdown.
- **Test `lord-netd` with a real physical NIC and Wi-Fi** on a system installed from the ISO (on VM 119 it was tested with a veth and `lord-sessiond` installed like the `.deb`).
- **ifupdown:** interfaces in `/etc/network/interfaces` show up as system ones, but `net-manage` doesn't yet know how to take them out of there.
