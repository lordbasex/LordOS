# LordOS development VM (Proxmox)

A reproducible log of every step applied to the VM. Rule: start from minimal Debian 13
and write down **every** package or change added here.

## Facts

| Field    | Value                                   |
|----------|-----------------------------------------|
| Host     | Proxmox VE 9.1 (`172.17.0.2`)           |
| VMID     | 115 — `lordos-dev`                      |
| IP       | `172.17.0.51/24`, gw `172.17.0.1`       |
| User     | `lord` (SSH key only, passwordless sudo) |
| Resources | 4 vCPU (`host`), 4 GB RAM, 32 GB disk  |
| Video    | `virtio` (virtio-gpu → `/dev/dri/card0`)|

Access: `ssh -i ~/.ssh/ssh/sshkey/lord.basex.pem lord@172.17.0.51`
Display: Proxmox noVNC console (VM 115).

## Step 1 — Create the VM (on the Proxmox host)

Base image: `debian-13-genericcloud-amd64.qcow2` (official Debian, no desktop and no
extra packages; ~320 base packages).

```sh
# /var/lib/vz/snippets/lordos-dev.yaml
#cloud-config
hostname: lordos-dev
users:
  - name: lord
    groups: [sudo, video, input, render]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: true
    ssh_authorized_keys: [ "<public key>" ]
package_update: true
packages: [qemu-guest-agent]
runcmd:
  - systemctl enable --now qemu-guest-agent
```

```sh
qm create 115 --name lordos-dev --machine q35 --cpu host --cores 4 --memory 4096 \
  --ostype l26 --agent enabled=1 --net0 virtio,bridge=vmbr0 \
  --scsihw virtio-scsi-pci --vga virtio --serial0 socket
qm importdisk 115 /var/lib/vz/template/iso/debian-13-genericcloud-amd64.qcow2 local-lvm
qm set 115 --scsi0 local-lvm:vm-115-disk-0 --boot order=scsi0 --ide2 local-lvm:cloudinit \
  --cicustom user=local:snippets/lordos-dev.yaml \
  --ipconfig0 ip=172.17.0.51/24,gw=172.17.0.1 --nameserver 8.8.8.8
qm resize 115 scsi0 32G
qm start 115
```

Result: Debian 13.7, kernel `6.12.111+deb13-cloud-amd64`, 323 packages.

## Step 2 — Generic kernel (needed for graphics)

The `-cloud` kernel **ships no DRM drivers**: there is no `/dev/dri`, so a Wayland
compositor can't start. It is replaced with Debian's generic kernel:

```sh
sudo apt-get install -y --no-install-recommends linux-image-amd64
sudo apt-get purge -y 'linux-image-*-cloud-amd64' linux-image-cloud-amd64
sudo reboot
```

Result: kernel `6.12.111+deb13-amd64`, `virtio_gpu` module loaded,
`/dev/dri/card0` and `/dev/dri/renderD128` present.

## Versions available in Debian 13 (verified)

| Package                 | Version     | Use                              |
|-------------------------|-------------|----------------------------------|
| `libwlroots-0.18-dev`   | 0.18.2      | base for the compositor in C     |
| `libwpewebkit-2.0-dev`  | 2.48.3      | embedded web engine (React shell) |
| `cog`                   | 0.18.4      | reference WPE launcher           |
| `libwebkitgtk-6.0-dev`  | 2.54.0      | alternative to WPE               |
| `nodejs`                | 20.19.2     | building the React shell         |
| `foot`                  | 1.21.0      | Wayland terminal for testing     |

> Note: these versions were surveyed before deciding on our own architecture. LordOS does **not**
> use wlroots, WebKit or cog (see `ARCHITECTURE.md`). `foot` will be useful to test M4.

## Step 3 — Toolchain (the only install for M1)

```sh
sudo apt-get install -y --no-install-recommends build-essential
```

It brings gcc 14.2, make, libc6-dev and linux-libc-dev (headers `drm/drm_mode.h`, `linux/input.h`).
Nothing else is needed: `lord-server` only links `libc` and `libm` (verified with `ldd`).
There is no `rsync` on the VM; the code is copied with `tar` over SSH:

```sh
COPYFILE_DISABLE=1 tar czf - --exclude build server | \
  ssh lord@172.17.0.51 'mkdir -p ~/LordOS && cd ~/LordOS && tar xzf - && make -C server'
```

## Step 4 — Test M1 and capture the screen

```sh
# on the VM
sudo ./build/lord-server --timeout 60 [--debug]
```

From the Proxmox host:

```sh
echo "screendump /tmp/lord.png -f png" | qm monitor 115          # real screenshot
python3 /root/lord-qmp-input.py 115 1280 800 move 600 190 down move 380 320 up
```

`tools/lord-qmp-input.py` (copied to the host's `/root/`) sends an **absolute** mouse over QMP
(`input-send-event`), just like noVNC. The HMP monitor's `mouse_move` doesn't work: it sends
relative motion, which QEMU's USB tablet ignores.

## Step 5 — Time zone (M2)

The theme's clock uses the system's local time:

```sh
sudo timedatectl set-timezone America/Argentina/Buenos_Aires
```

## Step 6 — Deploy and run M2

On the development machine: `npm install && npm run build` (generates `themes/default/theme.bundle.js`). Then:

```sh
COPYFILE_DISABLE=1 tar czf - --no-xattrs --exclude build --exclude node_modules --exclude legacy \
  server third_party assets themes | ssh lord@172.17.0.51 'cd ~/LordOS && tar xzf - && make -C server -j4'
# on the VM, from ~/LordOS/server:
sudo ./build/lord-server [--profile]      # --profile: per-primitive timings and % repainted
```

No new package was installed for M2: QuickJS, stb and Inter are inside the repo.

## Step 7 — M3: user folders and deployment

Standard folders so Files has content (the server can only read `$HOME`):

```sh
mkdir -p ~/Escritorio ~/Documentos ~/Descargas ~/Imágenes ~/Música ~/Videos
```

Deployment: same as step 6. There is no need to copy `apps/`, `sdk/` or `runtime/` to the VM
because they are inside each theme's bundle; do copy the whole `themes/`. `sudo ./build/lord-server` starts as
root and switches to `lord` (visible in the log: `running as lord (uid 1000)`). User settings:
`~/.config/lordos/settings.json`.

QMP testing: `tools/lord-qmp-input.py` now accepts `rdown`/`rup` (right click) and
`wheelup`/`wheeldown`. Keyboard: `echo "sendkey f4" | qm monitor 115` (HMP `sendkey`
sends scancodes; with the latam layout, `semicolon` = ñ and `bracket_left` = dead acute accent).

No new package was installed for M3.

## Step 8 — M4: native terminal

No package was installed: the terminal is part of `lord-server` and uses the `bash` Debian
already ships. The whole `assets/` (JetBrains Mono fonts and
`assets/shell/lord-bash.sh`) has to be copied in addition to `server/` and `themes/`.

Headless emulator test (inside the VM):

```sh
make -C server test
printf '123456789日本語\r\n' | server/build/vt-dump 10 3 7 3    # reflow with wide characters
```

Typing into the VM from the Proxmox host (`tools/lord-type.py`, copied to `/root/`; it needs
the US layout in LordOS, the ES/US button in the menu bar):

```sh
python3 /root/lord-type.py 115 'ls --color=always\n'
python3 /root/lord-type.py 115 '{meta_l-d}'          # Super+D: split pane
```

The monitor's `sendkey` drops keys in long bursts: the script sends them in batches of 40.

Devices detected on the VM: AT keyboard (`event0`), VMMouse (`event1`/`event2`) and
QEMU USB Tablet (`event3`, the one the noVNC console uses).

![M1 on the VM](m1-first-frame.png)

## Step 9 — M4: full terminal (fonts, shells, tmux)

Packages installed **only to test** Terminal features (LordOS doesn't need them
to work; if they are missing, those features aren't offered or fall back):

```sh
sudo apt-get install -y --no-install-recommends zsh fish tmux \
  fonts-noto-color-emoji fonts-dejavu-core fonts-firacode
```

| Package | What for |
|---|---|
| `zsh` 5.9, `fish` 4.0.2 | testing the integration with those shells (`sudo chsh -s /usr/bin/zsh lord`; go back with `/bin/bash`) |
| `tmux` 3.5a | testing `tmux -CC` |
| `fonts-noto-color-emoji` | color emoji (CBDT) |
| `fonts-firacode`, `fonts-dejavu-core` | choosing another monospace font, Fira Code ligatures, symbol fallback |

A minimal `~/.zshrc` was created for zsh (without one, zsh opens its first-run wizard).

Restarting the server **without losing the terminals** (the shells stay in their holders):

```sh
sudo pkill -x lord-server        # SIGTERM: saves screens and leaves the shells alive
cd ~/LordOS/server && sudo setsid -f sh -c "./build/lord-server > /tmp/lord.log 2>&1 < /dev/null"
ls /run/user/1000/lordos-term/   # one .sock (and .state) per live session
```

Terminal profile import test files in `~/Descargas`: `Dracula.itermcolors` and
`Catppuccin Mocha.itermcolors` (from a popular collection of terminal color schemes), a `Profiles.json` and
a binary `com.googlecode.iterm2.plist` in the same preferences format.

## Step 10 — M5: language and region

```sh
sudo apt-get install -y --no-install-recommends libxkbcommon-dev xkb-data libx11-data
```

| Package | Version | What for |
|---|---|---|
| `libxkbcommon-dev` (brings `libxkbcommon0`) | 1.7.0 | keyboard: lord-server links `-lxkbcommon` |
| `xkb-data` | 2.42 | every keyboard layout |
| `libx11-data` | 1.8.12 | data only: compose tables (dead keys) in `/usr/share/X11/locale` |

Installed **from LordOS** (Settings → Language & Region, through the privileged helper) during
the tests: the `pt_BR.UTF-8` and `es_AR.UTF-8` regions (`locale-gen`), `/etc/default/locale` =
`es_AR.UTF-8`, and the packages `manpages-pt-br`, `hunspell-pt-br`, `aspell-pt-br`, `hyphen-pt-br`
and `mythes-pt-br`. The time zone was tested by changing it to America/Sao_Paulo and set back to
America/Argentina/Buenos_Aires.

The helper (a `lord-server` child that stays root) only exists if the server was started with
sudo; the user `lord` is in the `sudo` group, which is what it requires to serve requests.

## Step 11 — M5b: complex scripts and input methods

```sh
sudo apt-get install -y fonts-noto-core fonts-noto-cjk skkdic rime-data-pinyin-simp
```

| Package | Version | What for |
|---|---|---|
| `fonts-noto-core` | 20201225-2 | Noto Sans/Naskh Arabic, Hebrew and the rest of the scripts (42 MB) |
| `fonts-noto-cjk` | 1:20240730 | Noto Sans CJK and Noto Sans Mono CJK (JP, KR, SC, TC) in `.ttc` (91 MB) |
| `skkdic` | 20230109-1 | Japanese SKK-JISYO.L dictionary (kana method) |
| `rime-data-pinyin-simp` | 0.0~git20241229 | simplified pinyin dictionary (pinyin method); brings `rime-data-stroke` |

They are data only (fonts and dictionaries), no new libraries. In a normal installation the
language pack installs them from Settings (Japanese: CJK fonts + skkdic; Chinese: + pinyin).
`iconv` for EUC-JP (SKK's format) already comes with `libc6`.

Tests on the VM (`make -C server test`):

```sh
./build/bidi-test BidiCharacterTest.txt      # https://www.unicode.org/Public/15.1.0/ucd/
./build/ime-test kana "toukyou_"             # _ = Space, ^ = Enter, < = Backspace
./build/shape-test /usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf ui arab 1 "مرحبا"
```

The comparison with HarfBuzz (`shape-test` against `uharfbuzz`) was run on the development
machine with the same fonts copied from the VM.

## Step 12 — M6: Linux apps (Wayland)

```sh
sudo apt-get install -y --no-install-recommends libwayland-dev wayland-protocols libwayland-bin
```

| Package | Version | What for |
|---|---|---|
| `libwayland-dev` (brings `libwayland-server0`) | 1.23.1 | lord-server links `-lwayland-server` |
| `wayland-protocols` | 1.44 | xdg-shell and xdg-decoration XML |
| `libwayland-bin` | 1.23.1 | `wayland-scanner`: generates the protocol code at build time |

Test apps (not needed for LordOS):

```sh
sudo apt-get install -y --no-install-recommends foot gtk-4-examples featherpad qt6-wayland
```

`foot` (Wayland terminal), `gtk4-demo` (GTK 4.18) and `featherpad` (Qt 6.8, with the Wayland
plugin from `qt6-wayland`). They are opened from a new tab in the LordOS Terminal (or over ssh
with `WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000`).

## Step 13 — M7: apps, packages and system services

**Sound** (`SOUND.md`):

```sh
sudo apt-get install -y --no-install-recommends pipewire pipewire-pulse wireplumber pipewire-alsa
sudo loginctl enable-linger lord
systemctl --user enable --now pipewire pipewire-pulse wireplumber
```

| Package | What for |
|---|---|
| `pipewire`, `pipewire-pulse`, `wireplumber` | sound server; lord-server talks to it over the PulseAudio protocol |
| `pipewire-alsa` | apps that use ALSA directly go through PipeWire |
| `pulseaudio-utils`, `sound-theme-freedesktop` | testing only (`pactl`, `paplay` and `.oga` sounds) |

**WebAssembly** (`WASM.md`), only to build the tests and the example app:

```sh
sudo apt-get install -y --no-install-recommends clang lld wabt git ca-certificates
```

`clang` and `lld` compile C to wasm32 (`tests/wasm/demo.c`, `examples/apps/fractal/src/mandel.c`);
`wabt` converts the official specification suite (`~/wasm-spec`).

**Clipboard**: `librsvg2-common`, testing only. Without it, `gtk4-demo --run=clipboard` doesn't
start, because its icons are SVG.

**Sound card** (to test with hardware and not only virtual outputs): in Proxmox,
`qm set 115 --audio0 device=ich9-intel-hda,driver=none` with the VM powered off; on the VM,
`sudo usermod -aG audio lord && sudo systemctl restart user@1000` (without a seated session,
`/dev/snd` only belongs to the `audio` group).

**Network and VPN** (`NETWORK.md`):

```sh
sudo apt-get install -y --no-install-recommends wpasupplicant openvpn
# tests only (access point, DHCP, radios and a WireGuard peer for comparison):
sudo apt-get install -y --no-install-recommends hostapd dnsmasq-base iw wireguard-tools
```

LordOS launches `wpa_supplicant` and `openvpn` with their fixed path and a config it generates. The
Wi-Fi tests use the `mac80211_hwsim` module and the VPN and DHCP tests use network namespaces: they never
touch eth0.

**Debian packages** (`PACKAGES.md`): nothing needs to be installed. Our own engine uses the
apt sources already there and `dpkg`, and keeps its index in `/var/lib/lordos/pkg`
(~98 MB with trixie, updates, backports and security).

**M5b scripts** (`LANGUAGES.md`): `libthai-data` and `fonts-noto-core` were already there;
`libthai-dev` only to compare Thai word breaking against libthai.

## VM 116 `lordos-gpu` (GPU path)

A full clone of 115 to test GPU composition without touching the development VM:

```sh
# on the Proxmox host
sed "s/^hostname: lordos-dev/hostname: lordos-gpu/" /var/lib/vz/snippets/lordos-dev.yaml > /var/lib/vz/snippets/lordos-gpu.yaml
qm clone 115 116 --name lordos-gpu --full
qm set 116 --vga virtio,memory=64 --ipconfig0 ip=172.17.0.52/24,gw=172.17.0.1 --cicustom user=local:snippets/lordos-gpu.yaml
qm start 116
# on the VM (172.17.0.52)
sudo apt-get install -y --no-install-recommends libgbm-dev libegl-dev libgles-dev libegl-mesa0 libgl1-mesa-dri
# tests only
sudo apt-get install -y --no-install-recommends mesa-utils mesa-utils-bin mesa-vulkan-drivers vulkan-tools glmark2-es2-wayland neverball
sudo usermod -aG kvm lord          # /dev/udmabuf for build/dmabuf-client
```

`vga: virtio-gl` (virgl) doesn't start on this host: it only has the ASPEED card, with no render node
(`libgl1` and `libegl1` were installed on the host while trying it). Inside the VM, Mesa uses llvmpipe
on 2D virtio-gpu. Details and measurements in `GPU.md`.

## Step 14 — M8: live ISO, GPU and test VMs

**Live ISO** (`ISO.md`). At first it was built on VM 115. Since the ISO with lord-sessiond
(October 16) it is built on **VM 116**, in `~/LordOS-iso`, because 115 is the user's
development VM:

```sh
sudo apt-get install -y --no-install-recommends debootstrap squashfs-tools xorriso grub-pc-bin \
  grub-efi-amd64-bin grub-common mtools dosfstools lintian libssl-dev
make -C server -j4 && sudo tools/iso/build.sh /var/tmp/lordos-iso      # ~8 min, 651 MB ISO
```

On VM 116 the following were installed on October 16: `libssl-dev`, to build `lord-sessiond` (required
by `mkdeb.sh`), and the ISO build packages above. `libgbm-dev libegl-dev libgles-dev`
were already there.

**GPU** (`GPU.md`): `libgbm-dev libegl-dev libgles-dev` to build with the GPU path.
Tests only: `mesa-utils mesa-vulkan-drivers vulkan-tools glmark2-es2-wayland
neverball`. On the Proxmox host `libgl1` and `libegl1` were installed when trying to use virgl;
it didn't work, because the host has no GPU.

**Test VMs on Proxmox:**

| VMID | Name | What for |
|---|---|---|
| 115 | `lordos-dev` | development (172.17.0.51) |
| 116 | `lordos-gpu` | clone of 115 for the GPU path (172.17.0.52, `vga: virtio`); the user `lord` is in the `kvm` group (`/dev/udmabuf`) |
| 118 | `lordos-notes` | clone of 115 to test Notes (172.17.0.53; snippet `local:snippets/lordos-notes.yaml`); `wl-clipboard` only for the clipboard tests |
| 116 | `lordos-gpu` | also, since M8: builds the live ISO (`~/LordOS-iso`, `/var/tmp/lordos-iso`) |
| 117 | `lordos-live` | boots the live ISO (4 GB, q35, `vga: virtio`, `audio0: ich9-intel-hda`, OVMF with efidisk or SeaBIOS). Boots from the CD first. It has a 24 GB `scsi0` disk with an old installation from an earlier test |
| 121 | `lordos-install-tmp` | temporary, already deleted: 24 GB, OVMF, `boot: order=scsi0;ide2`; the ISO was installed there and the installed system's login screen was tested (`qm destroy 121 --purge 1 --destroy-unreferenced-disks 1`) |

ISOs in `local:iso`: `lordos-live-0.7.0-amd64.iso` is the current one. Earlier ones are kept with
their date, for example `lordos-live-0.7.0-amd64-20261016-1529.iso`, the last one with `lordos-session`.

Test scripts in the host's `/root`, besides `lord-qmp-input.py`:
- `lordos-latam-type.py`: like `lord-type.py`, but for the latam layout, which is the ISO's.
  `/` is `shift-7` and `-` is `slash`.
- `lordos-typecmd.sh <vm> <file>`: types one line taken from a file, to avoid fighting with
  quoting.
- `lordos-grubsel.sh <vm> <n> <screenshot>`: reboots the VM and picks grub entry `n`. It sends
  `home` repeatedly to stop the countdown.

**VM 119 `lordos-spot`** (Search and clipboard, `SPOTLIGHT.md`): a full clone of 115
(`qm clone 115 119 --name lordos-spot --full`), IP 172.17.0.54 via cloud-init (snippet
`local:snippets/lordos-spot.yaml`, the same as 116's with another hostname) and a new MAC. Tests
only: `wl-clipboard` (`wl-copy`/`wl-paste`) and `tests/clip_secret.c`
(`cc -o clip-secret tests/clip_secret.c -lwayland-client`).

## Step 15 — M8: sessions, login screen and remote desktop

See `SESSIONS.md` and `REMOTE.md`. On VM 118 (172.17.0.53):

```sh
sudo apt-get install -y --no-install-recommends libssl-dev      # to build lord-sessiond
sudo apt-get install -y --no-install-recommends libgbm-dev libegl-dev libgles-dev libgl1-mesa-dri   # GPU path
make -C server -j4                                              # lord-server and lord-sessiond
# test install, root-owned (lord-sessiond only starts sessions with root-owned binaries):
sudo install -d /usr/local/lib/lordos/bin
sudo install -m 755 -o root -g root server/build/lord-sessiond server/build/lord-server /usr/local/lib/lordos/bin/
sudo cp -r themes assets /usr/local/lib/lordos/ && sudo chown -R root:root /usr/local/lib/lordos
```

- `lordos.service` (`/etc/systemd/system`): `ExecStart=/usr/local/lib/lordos/bin/lord-sessiond`,
  `ExecReload=/bin/kill -HUP $MAINPID`, `KillMode=process`, `Conflicts=getty@tty1.service`.
  Enabled.
- The first version's `lordos-remote.service` and the hand-launched `lord-server` were removed.
- `/etc/default/keyboard` with `XKBLAYOUT="us"`: it is the login screen's layout.
  Without that file it uses `latam`, LordOS's.
- **Test users:**
  - `bob` (`useradd -m -s /bin/bash -c "Bob Prueba" bob`);
  - the passwords of `lord` and `bob` are test-only, for logging in through the monitor and the web.
- `lordos-greeter` is created by `lord-sessiond`.
- The card is `virtio_gpu` without 3D: by default the monitor's session composites on the CPU, just like
  the old lord-server. To test the GPU path with llvmpipe,
  `/etc/systemd/system/lordos.service.d/gpu.conf` with `Environment=LORD_GPU=1` is used.
- `/etc/lordos/remote/config`: `enabled=1` after the Settings → Sharing test,
  `port=5673`.

**VM 119** (172.17.0.54) as the client:

- `chromium`, to test from a real browser;
- the scratchpad's test clients (`ws_test.py`), which speak TLS and WebSocket with
  Python's standard library.

No port or firewall rule was opened: the VMs are on the same LAN.

## Step 16 — M8: privacy, isolation and app signing

See `PRIVACY.md` and `SIGNING.md`. A VM of its own, a full clone of 115:

```sh
# on the Proxmox host
sed "s/^hostname: lordos-spot/hostname: lordos-sandbox/" /var/lib/vz/snippets/lordos-spot.yaml \
  > /var/lib/vz/snippets/lordos-sandbox.yaml
qm clone 115 122 --name lordos-sandbox --full
qm set 122 --ipconfig0 ip=172.17.0.56/24,gw=172.17.0.1 --cicustom user=local:snippets/lordos-sandbox.yaml
qm start 122
```

| VMID | Name | What for |
|---|---|---|
| 122 | `lordos-sandbox` | M8 privacy/signing (172.17.0.56); clone of 115, with `lord-sessiond` installed (`Delegate=yes`) |

- **Kernel**: `6.12.111+deb13-amd64` with Landlock ABI 6 (`/usr/include/linux/landlock.h`), seccomp
  and cgroup v2 (`cpu memory pids` delegated in `user@1000.service`). Nothing had to be installed
  for isolation: it is all kernel and libc.
- `libssl-dev` only to build `lord-sessiond` (as on 118); `lord-server` doesn't link it.
- **`lord-sessiond` with delegated cgroups** (as on 118, plus `Delegate=yes`):

  ```sh
  sudo install -d /usr/local/lib/lordos/bin
  sudo install -m 755 -o root -g root server/build/lord-sessiond server/build/lord-server /usr/local/lib/lordos/bin/
  sudo cp -r themes assets /usr/local/lib/lordos/ && sudo chown -R root:root /usr/local/lib/lordos
  echo "AUTOLOGIN=lord" | sudo tee /etc/lordos/session.conf
  ```

  `/etc/systemd/system/lordos.service`: the same as on 118 (`ExecStart=/usr/local/lib/lordos/bin/lord-sessiond`,
  `KillMode=process`, `Conflicts=getty@tty1.service`) **plus `Delegate=yes`**; enabled.
  `assets/` also carries `app-host.bundle.js` (the "app host" of isolated apps).
- **Users and remote desktop, tests only:** `bob` (`useradd -m -s /bin/bash -c "Bob Prueba" bob`);
  `lord` and `bob` have a test password (letters and digits, so it types the same with the
  login screen's latam layout); `/etc/lordos/remote/config` with `enabled=1`,
  `port=5673`. Browser logins were tested with the `ws_test.py` client (TLS +
  WebSocket with Python's standard library).
- **Without `lord-sessiond`** (a standalone `lord-server` with sudo, as in development) there is no delegated
  subtree: apps run without a cgroup, with a warning (expected fallback).
- **Test apps installed:** `tests/lpk-evil` (unsigned) and `tests/lpk-legit` (signed with the
  development key), plus Fractal; FeatherPad and foot were already there (from 115).
- **Tests** (`make -C server test` and separately): `build/sign-test` (55/55), `build/pkgstate-test`
  (11/11, with a test `.lpk`) and `build/sandbox-test` (13/13 app, 13/13 with network, 17/17
  `helper`; it needs `XDG_RUNTIME_DIR` for the Wayland socket test).
- **Sync** from the development machine with a script with its own name (`sync-sandbox-122.sh`), never a
  generic one: another agent once sent its tree to this VM through a shared `sync.sh`.
- **Development signing key**: on the development machine, `~/.config/lordos-dev/lordos-dev.key` (private, never
  committed); the public one is in `packaging/keys/lordos-dev.pub` and in `server/src/sec/keys.h`.

Do not touch VMs 115, 117, 118, 119, 120 or 123.

## Step 17 — M8: Monitor

See `MONITOR.md`. A VM of its own, a full clone of 115:

```sh
# on the Proxmox host
sed "s/^hostname: lordos-spot/hostname: lordos-monitor/" /var/lib/vz/snippets/lordos-spot.yaml \
  > /var/lib/vz/snippets/lordos-monitor.yaml
qm clone 115 123 --name lordos-monitor --full
qm set 123 --ipconfig0 ip=172.17.0.55/24,gw=172.17.0.1 --cicustom user=local:snippets/lordos-monitor.yaml
qm start 123
```

| VMID | Name | What for |
|---|---|---|
| 123 | `lordos-monitor` | M8 Monitor (172.17.0.55, MAC `BC:24:11:03:28:0F`); clone of 115 with `lord-sessiond` as on 118 |

On the VM (172.17.0.55):

```sh
sudo apt-get install -y --no-install-recommends libssl-dev     # lord-sessiond
sudo apt-get install -y --no-install-recommends libgbm-dev libegl-dev libgles-dev libgl1-mesa-dri   # GPU path (LORD_GPU=1 test)
sudo apt-get install -y --no-install-recommends bc             # only for the measurements (~/cpu.sh)
make -C server -j4 && make -C server test
sudo install -d /usr/local/lib/lordos/bin /etc/lordos
sudo install -m 755 -o root -g root server/build/lord-sessiond server/build/lord-server /usr/local/lib/lordos/bin/
sudo cp -r themes assets /usr/local/lib/lordos/ && sudo chown -R root:root /usr/local/lib/lordos
```

- **`lordos.service`:** the same as on 118 (`ExecStart=/usr/local/lib/lordos/bin/lord-sessiond`,
  `KillMode=process`, `Conflicts=getty@tty1.service`). It is enabled.
- **Login:**
  - `/etc/lordos/session.conf` with `AUTOLOGIN=lord`: it logs in by itself after every reboot, for
    the screenshots;
  - `/etc/default/keyboard` with `XKBLAYOUT="us"`.
- **GPU:** `/etc/systemd/system/lordos.service.d/gpu.conf` (`LORD_GPU=1`) was used once to
  test the GPU path with llvmpipe and removed. The measurements are on the CPU.
- **Tests only** (nothing installed):
  - real connections with `curl --limit-rate 30k` to Debian mirrors in several countries
    (`ftp.<country>.debian.org`);
  - Firefox ESR (already on 115) opening `www.bbc.co.uk`;
  - load with `dd` and shell loops.
- **Measurement scripts:**
  - `~/cpu.sh SEC`: CPU % of `lord-server`;
  - `~/thr.sh SEC`: the same, per thread.
- **No network touched:** no port was opened and `eth0`'s network wasn't changed (only the IP via
  cloud-init when cloning).

Do not touch VMs 115, 117, 118, 119, 120 or 122.

## Step 18 — Touchpad, gestures and touchscreens (VM 123)

See `TRACKPAD.md`. On VM 123 (172.17.0.55):

- `chromium` (155, from Debian) was already on the VM and was used to test pinch over Wayland;
- `/dev/uinput` comes with Debian's kernel;
- `linux-perf` was installed (`apt-get install --no-install-recommends linux-perf`), only to measure
  where `lord-server` spends CPU while moving the map (`perf record --call-graph dwarf`);
- `tpsim play --mouse ...` simulates a high-resolution wheel mouse; hot plugging and unplugging
  is tested with `tpsim play` (it creates the device, plays back and removes it);
- Fractal 1.1.0 (`examples/apps/fractal`, unsigned) was installed from Files → Store, to
  test the gestures in an isolated app.

Tests only:

```sh
make -C server -j4 && make -C server test && server/build/tp-test
cc -O2 -Wall -Wextra -o tests/touchpad-sim/tpsim tests/touchpad-sim/tpsim.c tests/touchpad-sim/gestures.c -lm
sudo install -m 755 tests/touchpad-sim/tpsim /usr/local/bin/tpsim
```

- **Test services**, with `Before=lordos.service` so `lord-sessiond` lends the
  simulated devices to the session at startup:
  - `tpsim-trackpad-123.service`: `tpsim serve /run/tpsim.fifo`;
  - `tpsim-ts-trackpad-123.service`: `tpsim serve --touchscreen /run/tpsim-ts.fifo`.

  They were left **disabled** at the end (`systemctl disable`). To test again,
  `sudo systemctl enable tpsim-trackpad-123` and reboot the VM.
- **Gestures:** `echo pinch-out > /run/tpsim.fifo`.
- **Screenshots:** from the Proxmox host, `echo "screendump /tmp/x.png -f png" | qm monitor 123`.
- **Pointer position:** the absolute mouse (QEMU's USB tablet), with
  `tools/lord-qmp-input.py 123 1280 800 move X Y`.
- **Remote desktop without a browser:** `RP_PINCH`/`RP_SCROLL` straight to the session socket
  (`/run/user/1000/lordos/remote.sock`), with a Python script from the scratchpad.
- **Network:** `eth0`'s network wasn't touched.

## Step 16 — M9: documents, printing and scanner (VM 116)

On **VM 116** (172.17.0.52), in a separate folder, `~/LordOS-m9`: `~/LordOS-iso` and its build
environment were not touched.

**To build `lord-doc`** (the documents helper, `DOCUMENTS.md`):

```sh
sudo apt-get install -y --no-install-recommends libpoppler-glib-dev libarchive-dev pkg-config
```

| Package | Version | What for |
|---|---|---|
| `libpoppler-glib-dev` (brings `libpoppler-glib8t64`, `libcairo2-dev`, `libglib2.0-dev`, FreeType and fontconfig) | 25.03.0 | `lord-doc`: PDF |
| `libarchive-dev` (brings `libarchive13t64`) | 3.7.4 | `lord-doc`: archives |

Without those `-dev` packages, `make -C server` builds everything else and skips `build/lord-doc` (`DOCHELPER=0`).

**For printing** (`PRINTING.md`):

```sh
sudo apt-get install -y --no-install-recommends cups printer-driver-cups-pdf cups-ipp-utils avahi-daemon avahi-utils
sudo usermod -aG lpadmin lord        # manage printers (LordOS administrators have it)
```

| Package | Version | What for |
|---|---|---|
| `cups` (`cups-daemon`, `cups-client`, `cups-filters`…) | 2.4.10 | the queue and the drivers; LordOS speaks IPP to it over `/run/cups/cups.sock` |
| `printer-driver-cups-pdf` | 3.0.2 | tests only: the virtual «PDF» printer (it leaves jobs in `~/PDF/`) |
| `cups-ipp-utils` | 2.4.10 | tests only: `ippeveprinter`, an IPP Everywhere printer "on the network" |
| `samba`, `nfs-kernel-server` | 0.8.7 | VM 125, for an hour on 2026-10-31: the test servers of Files → Network, inside a network namespace, with every unit masked so nothing listened on the LAN; purged afterwards (`FILES.md`, "Network") |
| `avahi-daemon`, `avahi-utils` | 0.8 | CUPS announces and finds printers; in the tests, announcing the scanner (`avahi-publish`). LordOS's mDNS doesn't need it |
| `libarchive-tools`, `zstd` | 3.7.4, 1.5.7 | tests only: building `.7z` and `.tar.zst` in `tests/doc_evil.sh` |
| `strace` | — | only for debugging the helper |

**Test emulators** (temporary systemd units, as the user `lord`):

```sh
sudo systemd-run --unit=lord-test-ipp --uid=lord ippeveprinter -p 8631 -M LordOS \
  -m "Impresora IPP de prueba" -l "Oficina 2" -f application/pdf,image/pwg-raster,image/jpeg \
  -d /tmp/ippeve "Impresora de la oficina"
sudo systemd-run --unit=lord-test-escl --uid=lord python3 -I ~/LordOS-m9/server/tests/escl_server.py 8090
sudo systemd-run --unit=lord-test-escl-mdns --uid=lord avahi-publish -s "Escáner de prueba" \
  _uscan._tcp 8090 "rs=eSCL" "ty=Escáner de prueba LordOS"
```

**The session on VM 116**, as in step 15:

- `lordos.service` (lord-sessiond) was installed with `Delegate=yes`;
- the binaries go in `/usr/local/lib/lordos/bin`: `lord-sessiond`, `lord-server` and
  **`lord-doc`**, which has to sit next to `lord-server`;
- `/etc/default/keyboard` with `XKBLAYOUT="us"`;
- `lord`'s password is test-only, for logging in through the login screen.

With `KillMode=process`, restarting requires stopping the sessions too:

```sh
sudo systemctl stop lordos; sudo pkill -x lord-server; sudo pkill -x lord-sessiond
sudo systemctl reset-failed lordos; sudo systemctl start lordos
```

Otherwise systemd doesn't start the service again («Device or resource busy»), because processes
are left in its cgroup.

**Test files** in `lord`'s home folder:

- `~/Documentos/WebAssembly-2.0.pdf`: 224 pages, with an outline and links;
- `~/Documentos/Formulario.pdf`: the form from `/usr/share/cups/data`;
- `~/Descargas`: the archives from `tests/make_evil_archives.py`;
- `Lobster-Regular.ttf`: an OFL font from google/fonts, for installing.
- Measurements: the file `~/.cache/lordos/doc-trace` turns on lord-doc's timing log.

## Step 19 — M9: media (VM 119 `lordos-spot`)

See `MEDIA.md`. VM 119 (172.17.0.54), a clone of the development VM, was set aside for media.

**Build** (`lord-media`; without these `-dev` packages, `make` builds everything else without it):

```sh
sudo apt-get install -y --no-install-recommends libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libswresample-dev libva-dev
```

At run time: `libavcodec61 libavformat61 libavutil59 libswscale8 libswresample5 libva2
libva-drm2`. For VA-API with AMD or Intel, also `mesa-va-drivers` (it doesn't help on the VM: 2D virtio-gpu
has no driver, and `vainfo` fails).

**Installed like the `.deb`:** `lord-sessiond`, `lord-server` and `lord-media` (root-owned) in
`/usr/lib/lordos/bin`, with the themes and `assets` in `/usr/lib/lordos`.
- `lordos.service` is the one from `tools/iso/rootfs`, without `ExecStartPre`, plus
  `lordos.service.d/delegate.conf` with `Delegate=yes`.
- `lord`'s test password is for the login screen.
- Remote desktop was turned on only for the protected-window test
  (`/etc/lordos/remote/config`) and went back to `enabled=0`.

**Tests only:**

| Package or change | What for |
|---|---|
| `ffmpeg`, `vainfo`, `v4l-utils` | building samples, feeding the virtual camera, inspecting formats |
| `linux-headers-amd64` + `v4l2loopback-dkms` | virtual camera. The headers brought kernel 6.12.113 (rebooted to use it) |
| `/etc/modprobe.d/lordos-test-cam.conf`, `/etc/modules-load.d/lordos-test-cam.conf` | `v4l2loopback devices=1 video_nr=10 card_label="Camara virtual (v4l2loopback)" exclusive_caps=1` |
| `/etc/udev/rules.d/70-lordos-test-video.rules` | `/dev/video*` owned by the `video` group (with a real webcam, logind/`uaccess` grants it) |
| transient service `lordos-test-camfeed` (`/usr/local/bin/lordos-test-camfeed`) | `ffmpeg` → `testsrc2` 1280×720 YUYV with the time → `/dev/video10` |
| `module-null-sink` `lordos_prueba` + `module-remap-source` `mic_virtual` («Micrófono virtual», default) and the `lordos-test-micfeed` service | virtual microphone with a looping clip (`paplay`) |
| `python3-pil` | `remote_shot.py` (scratchpad): a remote desktop client that assembles the screen into a PNG |
| `gdb` | finding the frame-queue overflow at the end of the file |
| `pulseaudio-utils`, `wl-clipboard` | `pactl` (streams and sources), `wl-paste` (clipboard) |

**Samples** (free licenses; details in `~/Música/LICENCIAS.txt`):
- `~/Videos/Big Buck Bunny (1080p).mp4` (+ `.srt`), `Prueba de sincronía.mkv` (with a subtitle
  track);
- two CC BY 3.0 EPs from archive.org in `~/Música`, with embedded cover art;
- `~/Imágenes/Muestras`: PNG, JPEG, WebP, BMP, HEIC, AVIF and an animated GIF;
- `~/media-src/sync.mp4`: the sync clip.

Measuring: `python3 server/tests/media_cpu.py 30` and `server/build/media-test play <file> …`.

The test services are transient and were left stopped. They are recreated with
`sudo systemd-run --unit=lordos-test-camfeed --uid=lord -p Restart=always /usr/local/bin/lordos-test-camfeed`
and the same with `lordos-test-micfeed`.

## Step 18 — Smooth windows, edges, cursors and sound (VM 118)

See `WINDOWS.md` and `SOUND.md` (Real time). On VM 118 (172.17.0.53):

- **Remote desktop off** (`/etc/lordos/remote/config`: `enabled=0`), so the session doesn't
  composite on the CPU because someone is watching.
- **`/etc/lordos/session.conf`: `AUTOLOGIN=lord`**, to restart the session with a script
  (`rm /run/lordos/autologin-done; systemctl restart lordos.service`).
- **Packages:**
  - **`rtkit`**, the same one the ISO ships. It served to confirm that with polkit it doesn't grant real time to
    `lord-sessiond`'s sessions.
  - **`chromium` 155** was already installed. It is opened in the session with
    `WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 chromium --ozone-platform=wayland
    --user-data-dir=~/.cache/chromium-ventanas`, for YouTube.
- **`lord` in the `pipewire` group** (`usermod -aG pipewire lord` and `systemctl restart user@1000`):
  real-time PipeWire, as in the ISO.
- **Virtual tablet for measuring:** `~/uinput-ventanas-118.py`, which runs as root. It creates the tablet
  «LordOS prueba tableta» and is driven through `/run/uinput-ventanas` (`move X Y`, `down`, `up`,
  `drag X Y sec hz radius`). It has to exist before the session takes the monitor.
  - The QMP mouse drops events above ~20 per second.
  - With `mouse_set 3` (QEMU monitor) the QMP mouse is the USB tablet and not the VMMouse.
- **Test files:**
  - `~/tono-ventanas.wav` (a 5-minute tone);
  - `~/antes-ventanas/` (the old themes, to measure the "before");
  - `~/LordOS-antes` (the tree without motion coalescing).
- **GPU path with llvmpipe:**
  `/etc/systemd/system/lordos.service.d/gpu-ventanas.conf` with `Environment=LORD_GPU=1`. It was added
  only for measuring and removed.

## Step 20 — M8: per-app firewall, `lord-netperm` (VM 122 `lordos-sandbox`)

See `FIREWALL.md`, step 2. On VM 122 (172.17.0.56), which already had `lord-sessiond` with
`Delegate=yes`:

- **Packages:** none new. `clang` 19 was already there and compiles `server/src/netperm/np.bpf.c`
  (`BPF=1` is set automatically when clang is present). Kernel `6.12.111+deb13-amd64` has `CONFIG_BPF_SYSCALL`,
  `CONFIG_CGROUP_BPF` and `CONFIG_BPF_JIT`. systemd-resolved 257 is running and `nsswitch.conf` uses
  `resolve`.
- **Installation:**

  ```sh
  make -C server -j4 && make -C server test
  sudo install -m 755 -o root -g root server/build/lord-netperm server/build/lord-server /usr/local/lib/lordos/bin/
  sudo install -m 644 lord-netperm.service /etc/systemd/system/      # the unit from FIREWALL.md (ExecStart in /usr/local/lib/lordos/bin)
  sudo systemctl daemon-reload && sudo systemctl enable --now lord-netperm
  ```

  The unit is the same one that goes into the ISO (the text was handed to the main session). It has
  `RestrictAddressFamilies=AF_UNIX AF_PACKET` (`AF_NETLINK` added in 0.8.3, for sock_diag), capabilities `CAP_BPF CAP_NET_ADMIN CAP_PERFMON
  CAP_DAC_READ_SEARCH CAP_NET_RAW`, `ProtectSystem=strict`, `ProtectHome=read-only`,
  `ProtectControlGroups=yes` and `RuntimeDirectory=lordos` with `RuntimeDirectoryPreserve=yes`.
- **Restarting `lordos.service`:** with `KillMode=process`, a `systemctl restart` leaves session
  processes behind and systemd can't start the service again ("Device or resource busy"). What was done:
  `systemctl kill -s KILL --kill-whom=all lordos`, `reset-failed` and `start`. To have it log in
  by itself again, `/run/lordos/autologin-done` was deleted. `lord-netperm` reattaches by itself
  to the new cgroup.
- **Tests only:**
  - `~/.local/share/applications/nptest.desktop` ("Prueba de red"): `foot -e ~/bin/nptest-loop.sh`,
    a loop of `curl` to example.com and www.debian.org and `/dev/tcp` to the gateway every 3 s;
  - `server/tests/netperm_integration.sh` (18/18), `server/tests/netperm_measure.sh`;
  - destinations: the gateway (TCP 53), 127.0.0.1, example.com and www.debian.org. Nothing large was
    downloaded from the internet.
- **No network touched:** neither eth0 nor nftables rules. The hooks are cgroup hooks and only
  reach the apps in LordOS sessions.
- **`systemd-run --user` escape (M8 follow-up, no new packages).** `lord-netperm` now also attaches
  to each session user's `user@<uid>.service` and applies the per-user restriction union there (see
  `FIREWALL.md`, «Escape via `systemd-run --user`»). Tested as the restricted app `desktop:aisla`
  ("no internet"): `systemd-run --user --scope curl`, `systemd-run --user curl` (service) and
  `setsid curl` all blocked, a normal process unaffected, and `bob`'s restriction not leaking into
  `lord`'s. Scratch scripts (outside the repo): `~/bin/escape-aisla-122.sh`,
  `~/union-escape-122.sh`, `~/bob-escape-122.sh`, `~/bin/run-escape-shot-122.sh` and
  `~/bin/escape-wait-122.sh`. The screenshot `docs/firewall-escape-blocked.png` is a `foot`
  window opened on the compositor (`WAYLAND_DISPLAY=wayland-0`) running the escape test inside the
  restricted scope, captured with `qm monitor 122` `screendump`. The lock screen was cleared by a
  full `lordos.service` restart with autologin (`rm /run/lordos/autologin-done`), not by changing
  any password.
- **Screenshots** with `qm monitor 122` (`screendump`) from the host. The mouse goes through
  `/root/lord-qmp-input.py 122`.
- **Frame and colors of Linux apps** (`WAYLAND.md`):
  - packages **`gnome-calculator`** (GTK 4/libadwaita) and **`mousepad`** (GTK 3), tests only;
  - `/etc/chromium/master_preferences` with `"custom_chrome_frame": false` (the original was kept as
    `master_preferences.orig-ventanas`);
  - `/etc/firefox-esr/lordos.js` with `pref("browser.tabs.inTitlebar", 0)`;
  - `assets/gtk/LordOS*` copied to `/usr/share/themes/`.


## Step — M9: Passwords and the system keyring (VM 116 `lordos-gpu`)

See `PASSWORDS.md`. Everything was built in a new folder, `~/LordOS-pw` (the workers used
`~/LordOS-pw-ctap`, `~/LordOS-pw-dbus`, `~/LordOS-pw-kdbx` and `~/LordOS-pw-qr`); `~/LordOS-iso`
and `~/LordOS-m9` were not touched.

- **Deploy:** `make -C server` in `~/LordOS-pw`, then `lord-server`, `lord-sessiond`,
  `lord-vault` (new) and `lord-doc` installed into `/usr/local/lib/lordos/bin` (root, 755), and
  `themes/` and `assets/` copied to `/usr/local/lib/lordos/`. Restarting the service needs the
  sessions killed first (`KillMode=process`): `systemctl stop lordos`, `pkill -x lord-server`,
  `reset-failed`, `start`.
- **Test user** `pwtest` (`useradd -m -s /bin/bash -c "Prueba Contraseñas" pwtest`, test-only
  password) so the login password path (`RP_KEYRING` → `login.lvault`) could be tested without
  touching `lord`'s password; added to `plugdev` for the FIDO2 rule.
- **Packages, test-only:**

  | Package | Version | Why |
  |---|---|---|
  | `libsecret-tools` (+ `libsecret-1-0`) | 0.21.7-1 | `secret-tool` against our Secret Service |
  | `chromium` | 155.0.8059.79-1~deb13u1 | saving and autofilling a login through the Secret Service |
  | `keepassxc-minimal` | 2.7.10+dfsg1-1 | creating the KDBX test files (`server/tests/import/`) |
  | `python3-venv` | 3.13.5-1 | a venv in `~/LordOS-pw-kdbx/venv` with pykeepass 4.2.0 for KDBX 4 variants |
  | `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libswscale-dev`, `libswresample-dev` | Debian 13 | building `lord-media`, so LordOS screenshots work for the protected-window capture |

- **FIDO2:** `/etc/udev/rules.d/71-lordos-fido.rules` (the rule in `PASSWORDS.md`); the software
  authenticator `server/build/softfido` (from `tests/fido-soft/softfido.c`) runs as root on
  `/dev/uhid` and shows up as `/dev/hidrawN` (`root:plugdev 0660`). No real key was available.
- **Fake USB stick:** `/var/tmp/pendrive-pw-116.img` (32 MB ext4, label `PENDRIVE`) loop-mounted at
  `/media/pwtest/PENDRIVE` (owned by `pwtest`) for the key-file screenshots.
- **Remote desktop** was turned on for a while (`/etc/lordos/remote/config`: `enabled=1`,
  `port=5673`) and turned off again afterwards (file removed). The fake USB stick is unmounted,
  `softfido`, the local web page and Chromium were stopped at the end.
- **Demo data:** `server/build/vault-demo lordos foto.jpg`, run as `pwtest`, creates a vault with
  invented items of every template (test-only master password `lordos`); `/tmp/sitio-pw/` is a
  local login page served on 127.0.0.1:8080 for the Chromium test.
- **Screenshots** with `qm monitor 116` (`screendump`) from the Proxmox host; mouse via
  `/root/lord-qmp-input.py 116`, keys via `sendkey` (`/root/lord-type.py`, and
  `/root/lordos-latam-type.py` once the session uses the latam layout).
- No network configuration was changed.

## Step 20 — App polish pass (VM 120 `lordos-video`)

See `DESIGN.md` and `docs/polish/`. VM 120 boots the **live ISO** (`lordos-live-0.7.0-amd64.iso`);
its disk is empty. Nothing persistent was changed: everything below lives in the live session's
RAM and disappears on reboot.

- **Network:** untouched (DHCP, it got `172.17.0.22`). No SSH or listening service was added.
- **Boot order:** set to `scsi0;ide2` for one boot to check the disk (empty), then restored to
  `ide2;scsi0`.
- **Deploying a build:** the tree goes into a raw `tar` file that is hot-plugged as a read-only
  USB disk from the Proxmox monitor (`drive_add` + `device_add usb-storage`, removed with
  `device_del`). It's installed from the session's Terminal (`sudo tar xf /dev/sdX -C /tmp` and
  `instalar-pulido-120.sh`, which copies `bin`, `themes` and `assets` to `/usr/lib/lordos` and
  restarts `lordos.service`).
- **Libraries:** the 0.7.0 ISO doesn't have the runtime libraries of the current `lord-doc` and
  `lord-media` (Poppler, libarchive, FFmpeg and their dependencies), so the PDF Viewer, Fonts and the
  Photos thumbnails failed (`libpoppler-glib.so.8: cannot open shared object file`). The libraries
  that were missing were copied from a Debian 13 container (`cp -n` into
  `/usr/lib/x86_64-linux-gnu`, then `ldconfig`). They aren't installed with `apt`.
- **Binaries:** built in a `debian:trixie` container with the `-dev` packages of steps 16 and 19.
- **Sample content** in `/home/lordos`: generated photos in `Imágenes` (with `Viajes` and `Familia`
  subfolders), `Documentos/Guia-LordOS.pdf` (5 pages with an outline), five notes in `Notas`, a
  small project in `Proyectos/lordos-demo`, and the Arabic test pack in
  `~/.local/share/lordos/locales/ar`.
- **Screenshots:** a scratch-only harness (never in the repo) bundled into the test theme. F9/F8
  step through 30 scenarios (6 apps × dark, light, RTL, minimum size, large), F7 opens a Terminal
  and F6 shows a glyph test. Captured with `screendump` from the Proxmox monitor.
- **Second pass (11 apps):** same workflow, with the server binaries rebuilt from the current tree
  in the `debian:trixie` container and a second scratch harness (12 targets × dark, light, RTL,
  minimum size; the emoji picker is opened with `emoji.open()`). Extra sample content in
  `/home/lordos`: generated, tagged MP3 albums with covers in `Música` (three artists), three test
  videos in `Videos` and three voice memos in `Música/Grabaciones` (renamed to `Recordings` for the
  Arabic scenario, whose UI falls back to English). Nothing is installed with `apt`.
- **Third pass (Settings page by page, Files, Terminal Preferences, Store):** same workflow, server
  binaries rebuilt from the tree of 2026-10-18 in the `debian:trixie` container, and a third scratch
  harness (122 scenarios; F10 skips one without opening it so a subset can be re-shot). The "after"
  build is the "before" snapshot plus only this batch's files, so other agents' unfinished work
  doesn't leak into the comparison. The live system has no Debian index, so the Store scenarios
  replace `lord.pkg` in the harness with sample data (16 apps, 5 other packages). Extra sample
  content: `Documentos/muestra.zip`. Nothing is installed with `apt`.

- **UI engine pass (ellipsis, per-side borders, transforms):** same workflow on the live session
  (`docs/WINDOWS.md`, "The UI engine pass"). Server binaries from the `debian:trixie` container; a
  scratch-only demo app and harness bundled into the test theme (F9 steps through Files in list and
  icon view in English and Arabic, the demo in LTR and RTL, the Dock's minimize effect slowed down to 6 s,
  and the Player); sample files with long English, Arabic, Hebrew, Thai and emoji names in
  `Documentos/Largos`. The drag was measured with a small static C uinput tablet (the live ISO has no
  `python3`) run with `sudo` from the Terminal, switching only `lord-server` between the two builds.
  Nothing is installed with `apt`. Once a React error in the scratch demo app (no `Icon` export) left the
  session black; `system_reset` from the Proxmox monitor brought the live ISO back and the build was
  deployed again.

## Step 21 — M8 app isolation: root-owned scopes, camera, several windows (VM 122 `lordos-sandbox`)

See `PRIVACY.md` («Root-owned app scopes», «Camera for isolated apps», «Several windows per
isolated app»). On VM 122 (172.17.0.56):

- **Packages:**
  - `v4l2loopback-dkms` 0.15.0-2 and `linux-headers-6.12.111+deb13-amd64` (the cloud kernel
    doesn't ship v4l2loopback; DKMS built and signed it);
  - `ffmpeg` 7.1.5 and `v4l-utils` 1.30.1, to feed and inspect the virtual camera;
  - `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libva-dev`, to
    build `lord-media` (as in step 19).
- **Virtual camera:** `/etc/modules-load.d/v4l2loopback.conf` (`v4l2loopback`) and
  `/etc/modprobe.d/v4l2loopback.conf` (`video_nr=10 card_label="Camara virtual (v4l2loopback)"
  exclusive_caps=1`) → `/dev/video10` (group `video`). The service
  `/etc/systemd/system/lordos-test-camfeed.service` (enabled) runs `ffmpeg -re -f lavfi -i
  testsrc2=size=1280x720:rate=30 -pix_fmt yuyv422 -f v4l2 /dev/video10`.
- **`lordos.service`:** added `DelegateSubgroup=supervisor` (as in the ISO's unit).
  `/usr/local/lib/lordos/bin` now also has `lord-media`.
- **Test apps and files (test only):**
  - `~/.local/share/applications/aisla.desktop` («Prueba de aislamiento»): `foot -e
    ~/bin/escape-aisla-122.sh`, which tries to leave its scope and checks the network before and
    after; `~/.config/lordos/firewall.json` restricts `desktop:aisla` (no internet, no incoming);
  - `tests/lpk-legit` 1.1.0 (camera, «New window») unpacked into
    `~/.local/share/lordos/apps/grabadora`;
  - `~/foto.jpg` and `~/video.mp4`, saved through the powerbox during the test.
- **Scripts:** `~/deploy-aisla-122.sh [full]` (installs the build; `full` stops/kills
  `lordos.service`, clears `/run/lordos/autologin-done` and starts it) and
  `~/vmbuild-aisla-122.sh` (runs `make`; while another agent's `src/hw` work was half done it added
  those sources and `-include stddef.h` to the VM's copy only). From the development machine:
  `sync-aisla-122.sh`, kept outside the repo.
- **Screenshots:** `qm monitor 122` (`screendump`) on the Proxmox host; the mouse through
  `/root/lord-qmp-input.py 122`, typing through `/root/lord-type.py 122`.
- **Network:** untouched (no eth0 change, no nftables).

## Step 22 — Settings: displays, Bluetooth, energy, date and time (VM 118)

See `SETTINGS.md`. On VM 118 (172.17.0.53), which already had `lord-sessiond` with
`AUTOLOGIN=lord` (step 18):

- **Tree and install:** a copy in `~/LordOS-ajustes` (sent with `tar` over ssh; the VM has no
  rsync), built there and installed as root into `/usr/local/lib/lordos/{bin,themes,assets}`; the
  session restarts with `systemctl kill -s KILL --kill-whom=all lordos`, `reset-failed`,
  `rm /run/lordos/autologin-done` and `start`.
- **Packages:**
  - **`bluez`** and **`bluez-test-tools`** (`btvirt`). `modprobe hci_vhci` and `btvirt -l2` (as
    root, in the background, log in `/tmp/btvirt-ajustes-118.log`) create two virtual controllers
    on one virtual radio: hci0 is the computer, hci1 plays a headset (alias "Auriculares de prueba",
    `btmgmt --index 1 class 4 4`, discoverable, a `bluetoothctl` agent fed through the FIFO
    `/tmp/btctl-ajustes-118.in`, script `~/btpeer-ajustes-118.sh`). Not persistent across reboots.
  - **`libspa-0.2-bluetooth`**: PipeWire's BlueZ support, so the headset shows up in Sound.
  - **`power-profiles-daemon`** (placeholder driver in the VM: power-saver and balanced).
  - `systemd-timesyncd` was already installed.
- **WirePlumber:** `~/.config/wireplumber/wireplumber.conf.d/lordos-bluez.conf` with
  `monitor.bluez.seat-monitoring = disabled` (lord-sessiond sessions are not logind seat sessions;
  without it WirePlumber never enables Bluetooth audio). The ISO needs the same file in
  `/etc/wireplumber/wireplumber.conf.d/`.
- **Files written by the tests:** `/etc/systemd/logind.conf.d/lordos.conf` (power button back to
  `poweroff`, lid `suspend`: logind's defaults), `~/.config/lordos/displays.json` (1280×800),
  `~/.config/mimeapps.list` (Firefox ESR as browser, Mousepad as text editor),
  `~/Escritorio/nota-ajustes.txt`. NTP was turned off and on again; the clock was set by hand once.
- **Screenshots** with `qm monitor 118` (`screendump`, converted to PNG on the development machine) and the mouse
  through `/root/lord-qmp-input.py 118`.

## Step 23 — M9 system apps and `lord-netd` (VM 119 `lordos-spot`)

See `SYSTEM.md` (Disks, Backups, Logs, Clock, Help) and `NETWORK.md` ("The network belongs to the
system"). VM 119 still runs LordOS installed like the `.deb` (step 19). It was not reinstalled from
the ISO.

**Deploy:** the tree is copied to `~/LordOS-sis` (only changed files, so `make` never keeps stale
objects), built with `make -C server -j4`, and `lord-server`, `lord-sessiond`, `lord-media` and the
new `lord-netd` are installed root-owned in `/usr/lib/lordos/bin`, with the themes and `assets`.

**Proxmox change (approved by main):** `qm set 119 --sata1 local-lvm:8`, an 8 GB SATA disk (`sdb`,
"QEMU HARDDISK" on AHCI). QEMU's SATA disk answers ATA SMART; the virtio-scsi `sda` doesn't. It
**stays** attached for the Disks tests. Remove it with `qm set 119 --delete sata1` and free
`local-lvm:vm-119-disk-1`. The boot order is unchanged (`scsi0`).

**Service:** `/etc/systemd/system/lordos.service.d/delegate.conf` now has
`DelegateSubgroup=supervisor` next to `Delegate=yes` (without it `systemctl restart lordos` failed
while processes survived; see `SESSIONS.md`).

**Packages:**

| Package | For |
|---|---|
| `exfatprogs` | Disks: format and label exFAT (`mkfs.exfat`, `exfatlabel`); also for the ISO |
| `ntfs-3g` | Disks: NTFS (optional; without `mkfs.ntfs` the app doesn't offer it) |

`dosfstools` and `e2fsprogs` were already installed.

**Test-only setup (not part of LordOS):**

| Item | For |
|---|---|
| `/usr/local/sbin/lordos-test-netns` + `lordos-test-netns.service` (enabled, `Before=lordos.service`) | netns `lnt` with `dnsmasq` (`10.77.0.50-99`, no router, `--leasefile-ro`) behind the veth `lnt0`, so `lord-netd` has a NIC of its own to bring up at boot; `eth0` (netplan) is never touched |
| `/etc/lordos/net/if/lnt0.conf` (`method=dhcp`) | the saved profile `lord-netd` restores at boot |
| `/etc/lordos/remote/config` `enabled=1` | remote desktop reachable before login (the test); set back to `enabled=0` after the tests |
| user `ana` ("Ana Prueba", created in Settings → Users, test password) | second user for the login/logout test and the home-folder check |
| `dummy_hcd` + `g_mass_storage file=/var/tmp/usb-disk-sistema-119.img removable=1 stall=0` | a real USB mass-storage stick inside the VM (sdc, TRAN usb, RM 1) for Disks; remove with `sudo rmmod g_mass_storage dummy_hcd` |
| `~/Descargas/lordos-prueba.img` (300 MB) | image for the bootable-USB test |
| ext4 image on a loop device under `/media/lord` | Backups destination during the tests (deleted afterwards) |

To undo the network test: `sudo systemctl disable --now lordos-test-netns`, `sudo ip netns del lnt`,
`sudo rm /etc/lordos/net/if/lnt0.conf /usr/local/sbin/lordos-test-netns
/etc/systemd/system/lordos-test-netns.service`.

**Screenshots** with `qm monitor 119` (`screendump … -f png`) and the mouse through
`/root/lord-qmp-input.py 119`, holding `flock /tmp/sistema-119-screen.lock` on the host while
driving the screen.

**Polish pass 4 (system apps, `docs/polish/{disks,backup,logs,clock,help}-*.png`):** a scratch-only
harness theme (never in the repo) was installed as `/usr/lib/lordos/themes/polish4`, chosen once in
Settings → Themes; it opened each app at a fixed frame in dark, light, Arabic (RTL) and minimum size
from a JSON command file, with no code evaluation. Sample content: the same journal entries
(`systemd-cat`) before each Logs capture, and a 3 GB ext4 image on a loop device mounted at
`/media/lord/CopiasPrueba` with two snapshots for Backups (`backup.json` saved and restored). Nothing
was installed. Afterwards the theme went back to `default` and the harness, the image, the loop
device and the temporary locale files were removed.

## Step 24 — M9 Mail, Calendar, Contacts and Reminders (VM 123 `lordos-monitor`)

Local test servers only, all on 127.0.0.1 inside the VM; no real accounts. Details in `MAIL.md`.

1. **Packages:** `dovecot-imapd dovecot-lmtpd postfix radicale apache2-utils python3-bcrypt openssl
   ca-certificates` (and `gdb` for debugging). To build `lord-maild`: `libssl-dev`.
2. **Names:** `127.0.0.1 mail.lordos.test dav.lordos.test lordos.test` in `/etc/hosts`.
3. **Test CA:** in `/etc/lordos-test-mail/`, a CA (`ca.crt`) and a server certificate for those
   three names (SAN, `extendedKeyUsage=serverAuth`), plus a self-signed `bad.crt` to try the
   certificate warning. The CA goes to `/usr/local/share/ca-certificates/lordos-test-mail-ca.crt`
   and `update-ca-certificates`. (It has no `keyUsage`: Python ≥ 3.13 clients need
   `ctx.verify_flags &= ~ssl.VERIFY_X509_STRICT`; OpenSSL in `lord-maild` accepts it.)
4. **Dovecot 2.4** (`/etc/dovecot/local.conf`): `listen = 127.0.0.1`, `protocols = imap lmtp`,
   virtual users from `/etc/dovecot/users` (passwd-file, SHA512-CRYPT, uid/gid 5000 `vmail`,
   `mail_home = /var/vmail/%{user | username}`, Maildir), `ssl = required`,
   `auth_allow_cleartext = no`, special-use mailboxes Drafts/Sent/Trash/Junk/Archive with
   `auto = subscribe`, LMTP and auth sockets inside Postfix's spool, and
   `protocol lmtp { auth_username_format = %{user | lower} }` (without it LMTP rejects
   `user@domain`). Raise `mail_max_userip_connections` (Mail opens one connection per account
   plus one per send).
5. **Postfix:** `virtual_mailbox_domains = lordos.test`, `virtual_transport =
   lmtp:unix:private/dovecot-lmtp`, `relay_transport = error:…` (nothing leaves the VM),
   submission on 587 (STARTTLS) and 465 (TLS) with Dovecot SASL, the test certificate.
6. **Radicale** (`/etc/radicale/config`): `127.0.0.1:5232`, `ssl = True` with the test
   certificate, `htpasswd` with bcrypt (`/etc/radicale/users`), `owner_only`, and
   `predefined_collections` for a calendar (VEVENT + VTODO) and an address book.
7. **Test users:** `ana@lordos.test` and `beto@lordos.test`, password `prueba123` (test only).
8. **Data:** scratch scripts (not in the repo) seed the conversations, a newsletter with remote
   images and a tracking pixel, a phishing-style message, an invitation, a 10,000-message folder,
   and Radicale events, to-dos and contacts with photos.
9. **Accounts in the session:** add them in Mail (*Add Account* → *Set the servers by hand*:
   `mail.lordos.test`, 993 / 465, and a CalDAV/CardDAV account
   `https://dav.lordos.test:5232/`). With autologin there is no keyring: Mail shows ⚠ next to the
   account; click it and type the password once per session.
10. **Tests:** `make -C server test` (includes `mail-test`, `mailc-test` and, with `libssl-dev`,
    `mailnet-test`); `make -C server mail-asan`.

## Step — Several displays and HiDPI (VM 118 `lordos-notes`)

See `DISPLAYS.md`.

- **Second virtual monitor** (Proxmox host, VM 118 only; needs a power cycle):

  ```
  qm set 118 --args "-global virtio-vga.max_outputs=2 -vnc unix:/run/lord-vm118-head1.vnc,id=head1,display=vga,head=1"
  ```

  The second head (Virtual-2) starts disconnected. An RFB client that sends ExtendedDesktopSize
  `SetDesktopSize W H` to `/run/lord-vm118-head1.vnc` plugs it in with that preferred mode; `0 0`
  unplugs it (scratchpad `head-monitors-118.sh`). Screendump of each head:
  `screendump file.ppm vga 0|1` in `qm monitor 118`.
- **Package** (tests only): `wayland-utils` (`wayland-info`, to check `wl_output`, xdg-output and
  fractional-scale).
- The GPU path was tested by adding `Environment=LORD_GPU=1` to `lordos.service` in a temporary
  drop-in, removed afterwards.

## Step 25 — Pre-ISO integration QA (VM 122 `lordos-sandbox`, October 18)

Clean build of the whole tree, the `.deb` installed with apt, and a smoke test from the login
screen. Results and screenshots: `docs/qa/` (`all-apps.png`, `settings-pages.png`, one PNG per app).

- **Packages** (build deps from `ISO.md`, "Build packages"): `libgbm-dev libegl-dev libgles-dev
  libssl-dev libpoppler-glib-dev libarchive-dev libavcodec-dev libavformat-dev libavutil-dev
  libswscale-dev libswresample-dev libva-dev clang lintian` (most were already there), plus
  `linux-headers-6.12.113+deb13-amd64`: an apt run had upgraded the kernel to 6.12.113 and DKMS needed
  the headers to rebuild `v4l2loopback` for the virtual camera (`/dev/video10`).
- **The package instead of `/usr/local`:** `tools/iso/mkdeb.sh /tmp/lordos-qa-122.deb` and
  `apt-get install /tmp/lordos-qa-122.deb` (this validates `Depends`). The VM's own units in
  `/etc/systemd/system` (`lordos.service`, `lord-netperm.service`, pointing at `/usr/local/lib/lordos`)
  were moved aside to `*.qa-122.bak` so the package's units in `/usr/lib/systemd/system` are used;
  both enabled. `/usr/local/lib/lordos` is left in place, unused.
- **Login screen instead of autologin:** `/etc/lordos/session.conf` is `AUTOLOGIN=` (the previous
  file is `session.conf.qa-122.bak`).
- **Test user:** `qa` («Qa Tester», standard), created from Settings → Users to check a new user's
  defaults.
- **Tree and scripts:** the tree is synced to `~/LordOS-qa` by the development machine's scratch script
  `sync-qa-122.sh`; screenshots via `qm monitor 122` (`screendump`), mouse via
  `/root/lord-qmp-input.py 122`, typing via `/root/lord-type.py 122`. Network untouched.
- **QA fixes (login screen, `make test` runner):** `unicode-data` installed with apt so
  `tests/run_suites.sh` finds `/usr/share/unicode/BidiCharacterTest.txt` (the bidi conformance suite).
  The fixes were built in a separate copy, `~/LordOS-qafix-122`. The new `lord-sessiond` and the
  greeter bundle are installed in `/usr/lib/lordos` (the package's originals are kept as
  `*.qafix-122.bak`). To test "last user", `qa` was shown once through a one-time
  `AUTOLOGIN=qa` (restored to `AUTOLOGIN=` right away) and its session was then ended, which follows
  the same path in `lord-sessiond` as a logout. `/var/lib/lordos/last-user` now exists.

## Step 26 — Firewall restrictions for Linux apps (VM 119, October 18)

See `FIREWALL.md`, "Linux apps". Tree in `~/LordOS-fwlinux`, built there (`make -C server -j8`,
`make -C server test`: 23 passed, 2 skipped).

- **Packages (tests only):** `strace` (the GTK app's file opens in `tests/fwlinux/files.sh`),
  `libnotify-bin` (`notify-send`, `notify.sh`), `libglib2.0-bin` (`gdbus`, the
  `StartTransientUnit` escape test), `ristretto` (an image viewer for the "what breaks" check),
  `alsa-utils` (`arecord`, `mic.sh`).
- **Installed:** `server/build/lord-netperm` in `/usr/lib/lordos/bin/` and
  `tools/iso/rootfs/etc/systemd/system/lord-netperm.service` in `/etc/systemd/system/`, enabled
  (this VM didn't have the service). The session's `lord-server` was not replaced: the launcher,
  notification and PipeWire code were tested with their test tools (`build/launch-cli`,
  `build/notifyd-cli`, `build/pwguard-cli`), which run the same source files.
- **Devices:** the VM's `v4l2loopback` camera (`/dev/video10`), its USB buses, `/dev/hidraw0`,
  `/dev/fb0` and the emulated sound card (`alsa_input…analog-stereo`).
- **Scripts:** `server/tests/fwlinux/{devices,bluetooth,files,notify,mic,screen}.sh`, run as `lord`.
  They build app scopes by hand as root under `lordos-session-1000` and put `firewall.json` back
  at the end. Nothing in the network configuration was touched.

## Step 27 — Remote login (SSH) in Settings → Sharing (VM 122 `lordos-sandbox`, October 18)

See `REMOTE.md`, "Remote login (SSH)". Tree in `~/LordOS-ssh` (sent with `tar`), only
`make -C server build/lord-server`; that `lord-server`, the three theme bundles, `app-host` and
`assets/locales` were installed over `/usr/lib/lordos` after saving the previous ones.

- **Packages:** `openssh-server` and `openssh-sftp-server` were purged with `apt-get` (test setup:
  a system without the SSH server, driven through `qm guest exec 122`) and installed again by the
  Settings switch through the package engine (`pkg-install`), same version
  (`1:10.0p1-7+deb13u4`).
- **Changes undone at the end:** the previous `/etc/ssh` (host keys and the cloud image's
  `sshd_config` with `PasswordAuthentication no`) was restored from a backup, both LordOS drop-ins
  were removed and `ssh.service` was enabled again, so other agents' SSH access and known host
  keys are unchanged. `lord` was taken out of `sudo` for the standard-user test and put back. The
  saved `lord-server`, themes and assets were restored and `~/LordOS-ssh` deleted.
- **Side effects:** the VM was rebooted twice (the reboot tests), which ended `bob`'s background
  session; `lord` ends at the login screen. Nothing in the network configuration was touched.

## Step 28 — Monitor → Network: traffic accounting, chart and map (VM 118 `lordos-notes`, October 18)

See `MONITOR.md` and `FIREWALL.md` ("Traffic accounting"). On VM 118 (172.17.0.53), in its own
folder `~/LordOS-monred`, installed into `/usr/local/lib/lordos` as in step 15 (the previous
`/usr/local/lib/lordos` was saved in `/var/tmp/lordos-backup-monred` and put back at the end).

- **Installed, tests only:** `iperf3` 3.18 (`apt-get install --no-install-recommends iperf3`; its
  service stays disabled). For the throughput and CPU measurements a veth pair (`npb0`/`npb1`)
  into a private network namespace `npb` (`10.213.7.0/30`) was created and deleted afterwards;
  eth0 and the VM's network configuration weren't touched.
- **Traffic:** Chromium (already on the VM) on a video site, `curl --http3-only` (Debian's curl
  8.14 has HTTP/3) and `curl --limit-rate` downloads from Debian mirrors, each run inside an app
  scope made by hand (`app-lordos-<name>.scope`, as `tests/netperm_integration.sh` does).
- **Screenshots** with `screendump` from the Proxmox host; the pointer with
  `lord-qmp-input.py 118 2987 1067` (the VM has two heads, see "Several displays").
- The session language, theme and accent were changed for the screenshots and restored from
  `~/.config-lordos-backup-monred` afterwards.

## Step 29 — 0.8.2: immutable (A/B) prototype (VM 122 `lordos-sandbox`, October 19)

See `IMMUTABLE.md`, "The prototype". On VM 122 (172.17.0.56), in its own folder `~/LordOS-inmut3`
(only `tools/immutable/`) and the work folder `/var/tmp/lordos-immutable` (base tree, images,
throwaway test key). The test images boot in **QEMU inside the VM** (nested KVM, OVMF): the VM's own
disk, boot, network and LordOS installation were not touched.

- **Installed** (`apt-get install --no-install-recommends`): `debootstrap`, `systemd-repart`,
  `systemd-ukify`, `systemd-boot-efi`, `systemd-boot-tools`, `cryptsetup-bin`, `erofs-utils`,
  `qemu-system-x86`, `ovmf`, `gpg`, `gpg-agent`, `gpgv`, `libcryptsetup12`, `mtools`,
  `dosfstools` (and their dependencies). `systemd-boot` itself and dracut were installed only inside
  the image's chroot, never on the VM (dracut would replace the VM's initramfs-tools).
- **Run:** `sudo tools/immutable/build.sh all` and `sudo tools/immutable/demo.py` (as root: `lord`
  is not in the `kvm` group, and it was not added). The demo's HTTP server listens on port 8642
  only while it runs; QEMU uses user-mode networking.
- **Left behind:** the packages above and `/var/tmp/lordos-immutable` (~5 GB; delete it to free the
  space). Nothing else changed.
## Step 30 — New wallpapers (VM 123 `lordos-monitor`, October 19)

Tree in `~/LordOS-wp`, built there (`make -C server -j4`, `make -C server test`: 23 passed,
3 skipped). Nothing was installed with apt.

- **Deployment, temporary:** `/usr/local/lib/lordos` was copied to `lordos.wp123-bak`, the new
  `lord-server`, `lord-sessiond`, `lord-vault`, `lord-maild`, the themes and `assets/` were installed
  over it, and the collection went to `/usr/share/backgrounds/lordos` with the
  `assets/wallpapers` link, as the package does. Afterwards the backup was copied back, the files
  that only the new build had were removed (`diff -rq` against the backup: identical), and
  `/usr/share/backgrounds/lordos` was deleted.
- **New user:** `~/.config/lordos/settings.json` was replaced by `{"language": "en"}` for the
  screenshots (Celestial by default, then Crystal Crown and light mode chosen in Settings) and
  restored from its backup (`wallpaper: "sunset"` again).
- **Login screen:** the session from before the deploy was ended and `/run/lordos/autologin-done`
  removed to show the greeter and then autologin again; `/etc/lordos/session.conf` wasn't changed.
- **Screenshots** with `screendump` from the Proxmox host, the pointer with
  `lord-qmp-input.py 123 1280 800`. eth0 and the VM's network weren't touched.
## Step 31 — Disk temperatures and SMART with smartmontools (VM 119 `lordos-spot`, October 19)

See `MONITOR.md` ("Disk temperature and health") and `SYSTEM.md` ("SMART with smartmontools").

- **Deploy:** the `v082-disks` tree built in `~/LordOS-dt082`; only `lord-server`, the themes and
  `assets/locales` were installed in `/usr/lib/lordos`, after a backup
  (`/var/tmp/disktemp082-backup/lordos-before.tgz`), and put back from it afterwards.
- **Test user:** `disktest` ("Disk Test", in the `sudo` group, a test-only password), created so the
  login screen could be used without touching `lord`'s password; removed afterwards
  (`userdel -r disktest`).
- **`drivetemp`** was loaded by hand (`modprobe drivetemp`) to check that QEMU's SATA disk gets no
  sensor (it has neither SCT nor attribute 194); unloaded afterwards.
- **smartmontools** was installed twice from the Disks app's "Install smartmontools" (the package
  engine's `pkg-install`), as the feature does, and purged afterwards (`apt-get purge
  smartmontools`), so the next test starts from "not installed" again. Before that, the fixtures in
  `server/tests/smart/` were captured with a `smartctl` unpacked into `/tmp/smx` (`apt-get download`
  + `dpkg -x`, never installed).
- Nothing else changed: no Proxmox setting, no network setting.
## Step 32 — Monitor → Network: per-destination switches, traffic history, several damaged rectangles (VM 119 `lordos-spot`, October 19)

See `MONITOR.md` ("Per-destination switches", "Traffic history", "0.8.2: several damaged
rectangles"). On VM 119 (172.17.0.54), built in its own folders (`~/LordOS-monred`, plus
`~/LordOS-before` from `origin/main` and instrumented copies for the measurements), all deleted at
the end.

- **Installed:** `linux-perf` (`apt-get install --no-install-recommends linux-perf`), to find where
  `lord-server` spends CPU while the map animates (`perf record -g -t <main thread>`). It stays
  installed.
- **Deployed for the test, then restored:** `/usr/lib/lordos` (the session's `lord-server`, the
  theme bundles, `assets`, and for the per-app history `lord-netperm` from the same tree, since the
  installed one predates the 0.8.1 traffic accounting) was saved in `/var/backups/lordos-monred-bak`
  and put back at the end (file list and contents compared with `diff -r`; the backup was then
  deleted). `lord-netperm.service` was restarted with the original binary.
- **Session:** `/etc/lordos/session.conf` with `AUTOLOGIN=lord` (it didn't exist; removed at the
  end) and `/run/lordos/autologin-done` removed before each `systemctl restart lordos`, to start a
  fresh session with each build. The language (English), Appearance mode and Reduce motion were
  changed for the screenshots and measurements, and `settings.json` and `firewall.json` were
  restored from a copy; the history file the test wrote (`~/.local/state/lordos`) was deleted. The
  VM ends at the login screen.
- **Traffic:** `curl --limit-rate 40k` loops to Debian mirrors in ten countries from the SSH
  session (stopped at the end), and Firefox ESR (already on the VM) opening a few sites and one
  download (`~/Downloads/ls-lR.gz`, deleted).
- **Screenshots** with `screendump` from the Proxmox host; pointer and keys with
  `lord-qmp-input.py 119 1280 800` and `qm monitor 119` `sendkey`.
- Nothing in the network configuration was touched (`eth0` keeps 172.17.0.54).
## Step 33 — Mail, Calendar, Contacts and Reminders in their own process (VM 118 `lordos-notes`, October 19)

See `PRIVACY.md` ("System apps in their own process"). On VM 118 (172.17.0.53), tree in
`~/LordOS-inmut2`, built there and installed into `/usr/local/lib/lordos` (`lord-server`,
`lord-sessiond`, and `lord-maild` and `lord-vault`, which this VM's install didn't have, plus themes
and assets) by `~/inst-inmut2-118.sh`; the previous tree was saved in
`~/inmut2-backup-20261019/lordos` and put back at the end (files that only the new build added were
removed, so `diff -r` against the backup is empty). Sessions were the autologin session of `lord`
(`/run/lordos/autologin-done` removed, then `systemctl restart lordos`).

- **Installed, tests only, then purged:** `dovecot-imapd` (with `dovecot-core`, `dovecot-sieve`,
  `libexttextcat-2.0-0`, `libexttextcat-data`, `libunwind8`), its service disabled before it ever
  ran (`policy-rc.d`), run by hand from `/tmp/dctest` on `127.0.0.1:1143/1993` with a throwaway
  self-signed certificate and one test user (`ana@lordos.test`, test-only password); a Python SMTP
  sink on `127.0.0.1:2465` (a transient unit, nothing relayed). `gdb` (with `libdebuginfod1t64`,
  `libdebuginfod-common`, `libpython3.13`, `libipt2`, `libsource-highlight*`, `libbabeltrace1`) to
  call `open()`/`socket()` inside the sandboxed Mail process. All purged afterwards; `/tmp/dctest`
  and the `dovecot`/`dovenull` users are gone.
- **Input:** a uinput tablet (`~/uinput-inmut2-118.py`, as a transient unit, now with right click)
  driven through `/run/uinput-inmut2`; screenshots with `screendump` from the Proxmox host.
- **Undone at the end:** `~/.config/lordos/settings.json` (English UI and `debugTiming` for the
  tests) restored from the backup, the Mail restriction in `firewall.json` removed, and the test
  data deleted (`~/.config/lordos/mail`, `~/.local/share/lordos/mail`, the four apps'
  `appdata` folders, two files in `~/Documents`). Nothing in the network configuration was touched.

## Step 34 — 0.8.3: X11 apps with Xwayland (VM 118 `lordos-notes`, October 19)

See `WAYLAND.md` ("X11 apps (Xwayland)") and `PRIVACY.md` ("X11 apps"). Installed with
`--no-install-recommends`:

| Package | What for |
|---|---|
| `xwayland` 2:24.1.6 | X11 apps (the only one LordOS needs; it brings `xserver-common`, `x11-xkb-utils`) |
| `xterm`, `x11-apps` (xeyes, xwd), `x11-utils` (xprop, xwininfo, xev) | test apps and X inspection |
| `xclip`, `xinput`, `xdotool` | clipboard tests, the keyboard-snoop test (`tests/xwayland/keysnoop.sh`) and reading the X pointer (XTEST is off, so `xdotool` can't type) |
| `openjdk-21-jdk-headless`, `openjdk-21-jre` | the Swing test app (`~/xwl-tests/SwingTest.java`, a `.desktop` with `StartupWMClass=SwingTest`, removed after the tests) |
| `neverball` | an SDL game (`SDL_VIDEODRIVER=x11`) |

- Build: `make -C server` (no new build dependency: the primary-selection XML is in
  `wayland-protocols`), plus `make -C server build/clip-offers` for `tests/xwayland/selections.sh`.
- The GPU path was tested with a temporary `lordos.service.d` drop-in `Environment=LORD_GPU=1`
  (llvmpipe), removed afterwards.
- Input: a uinput tablet+keyboard (`~/uinput-xwl-118.py`, the same as the earlier steps' scripts)
  mapped to the whole desktop (2987×1067 on this VM's two displays).
- The installed LordOS was backed up to `~/xwayland-backup-20261019` and restored after the tests;
  `firewall.json` was restored too.

## Step 35 — The Classic theme as a second full appearance (VM 119 `lordos-spot`, October 19)

See `DESIGN.md` ("Lord Classic") and `WINDOWS.md` ("Snapping"). On VM 119 (172.17.0.54), tree in
`~/LordOS-classic`, built there (`make -C server`, `make -C server test`) and installed into
`/usr/lib/lordos` (`lord-server`, `lord-media`, `lord-vault`, `lord-maild`, the theme bundles, the
app host, the system apps, `assets/{fonts,locales,shell,icons,mascot,monitor,help}` and the wallpaper
JPEGs under `assets/wallpapers`, which this older install didn't have) by `~/deploy-classic-119.sh`.
The installed tree was saved first in `/var/backups/lordos-classic-bak` and put back at the end.

- **Nothing installed with apt.** No Proxmox or network setting was touched (`eth0` keeps 172.17.0.54).
- **Session:** `/etc/lordos/session.conf` with `AUTOLOGIN=lord` (it didn't exist; removed at the
  end). A fresh session needs the old one's `lord-server` ended first, then `/run/lordos/autologin-done`
  removed and `systemctl restart lordos` (restarting the service alone keeps the open session).
- **Screenshots and measurements:** a scratch-only harness (never in the repo) was built in front of
  both themes' bundles: it read `~/.cache/lordos-shot.json` and opened one app at a centered frame,
  switched theme, mode or language. Screenshots with `screendump` from the Proxmox host, the pointer
  with `lord-qmp-input.py 119 1280 800`, keys with `qm monitor 119` `sendkey`, holding
  `flock /tmp/sistema-119-screen.lock`. Window drags for the frame times with a QMP script in the
  host's `/tmp` (deleted afterwards); lord-server's own `drag:` lines in `~/.cache/lordos/session.log`.
- **Test notifications** with `notify-send` (they live in the session's memory only).
- **Undone at the end:** `/usr/lib/lordos` put back from the backup (the files the deploy added
  removed, the rest copied back; `diff -r` against the backup empty, then the backup deleted),
  `~/.config/lordos/settings.json` restored from its copy, `~/.cache/lordos-shot*.json`,
  `~/shot-harness`, `~/deploy-classic-119.sh` and `~/LordOS-classic` deleted. The VM ends at the
  login screen.

## Step 36 — Remote desktop security review (VM 122 `lordos-sandbox`, October 19)

See `REMOTE.md`, "Security analysis (0.8.3)". On VM 122 (172.17.0.56), tree in
`~/LordOS-remotesec`, built there (`make -C server`, `make -C server test`) and installed over the
package's files by `~/deploy-remotesec-122.sh` (it saved `/usr/lib/lordos`, `/etc/lordos` and
`/var/lib/lordos/remote` to `/var/backups/lordos-remotesec-20261019` first).

- **Nothing installed.** The attacks ran from the development Mac (stdlib Python client in the
  scratchpad, a throwaway headless Chrome profile for the screenshots) and from the Raspberry Pi
  (the same Python script over SSH stdin); OpenSSL's `s_client` on the VM did the TLS scan.
- **Test-only account `rtest`** (password only for these tests), temporarily in `sudo` and the
  autologin user, so the block notification reached the monitor's session; removed at the end.
- `~/unblock-remotesec-122.sh <cidr>` wrote the same request as the `remote-unblock` action.
- **Monitor and Settings screenshots** were taken through the remote desktop itself, in that
  headless browser (the session was `rtest`'s, shown on the monitor so the notification reached it).
- **Undone at the end:** `/usr/lib/lordos` and `/etc/lordos` restored from the backup (`diff -r`
  empty; `session.conf` back to `AUTOLOGIN=`, the remote config as before: `enabled=1`, port 5673),
  `/var/lib/lordos/remote` and `/run/lordos/remote-failures` removed, `/var/lib/lordos/last-user`
  back to `lord` (the autologin had changed it), `rtest` and its home deleted, the tree, the scripts
  and the backup removed, `lordos.service` restarted on the package's 0.8.2 binaries, at the login
  screen. Network untouched.

## Step 37 — 0.8.3 release test (VMs 116, 117, 120 and the Raspberry Pi, October 19)

See `ISO.md`, "Release test (0.8.3 Beta, October 19)".

- **VM 116:** the release tree in `~/LordOS-iso`, built there; ISO and `.deb` in
  `/var/tmp/lordos-iso/out/` (the 0.8.0 and 0.8.1 ISOs there were removed to make room: identical
  copies are in Proxmox's `local:iso`). Nothing installed.
- **Proxmox:** `local:iso/lordos-live-0.8.3-amd64.iso` added. A temporary typing helper in the
  host's `/root` was removed afterwards.
- **VMs 117 and 120:** a Proxmox snapshot `r083pre` first; then 120 got a fresh 0.8.2 install from
  the 0.8.2 ISO (test user `rtester`, `openssh-server`, a sudoers drop-in, `lordos.sources` on
  `apt-staging`, smartmontools, xterm) and was upgraded twice (Store, then apt from a second
  snapshot), and 117 installed the 0.8.3 ISO. Both were rolled back to `r083pre` (ISO, boot order
  and disks as before: the earlier "Test User" installs, at the login screen) and the snapshots
  deleted.
- **Raspberry Pi 5 (172.17.0.24):** built the arm64 package and the image in
  `/var/tmp/lordos-src-083` and `/var/tmp/lordos-rpi-v083b`. The installed LordOS went from 0.8.0
  to 0.8.3 with apt: `/usr/share/keyrings/lordos-archive-keyring.gpg` and
  `/etc/apt/sources.list.d/lordos.sources` (the live repository) added, `lordos` 0.8.3 with
  `xwayland` and its 14 dependencies installed, migrations 001–004 applied (002 wrote
  `/etc/ssh/sshd_config.d/09-lordos-root-keys.conf`, so root keeps logging in with keys only), and
  `lordos.service` restarted (only the login screen was running). No reboot; network, the
  `lordbasex` account and the SD partitions untouched.

## Step 38 — 0.8.4: browsers with their own bar (VM 119 `lordos-spot`, October 19)

See `WAYLAND.md` ("Who draws the frame", "The browsers by default"). On VM 119 (172.17.0.54), tree
in `~/LordOS-csd`, built there (`make -C server`, `make -C server build/deco-client`,
`make -C server test`) and installed over `/usr/lib/lordos` by `~/deploy-csd-119.sh` (it saved
`/usr/lib/lordos`, the original `/etc/chromium/master_preferences` and `settings.json` first, in
`/var/backups/lordos-csd-bak`).

- **Installed for the tests:** `gsettings-desktop-schemas` (the ISO has it; without it GTK 3 can't
  read the theme and the button layout from the keyfile backend, and Chromium's GTK theme shows
  Adwaita's buttons on the right), removed at the end. `assets/gtk/LordOS*` copied to
  `/usr/share/themes/` (the package does that), removed at the end.
- **Session:** `/etc/lordos/session.conf` with `AUTOLOGIN=lord` (it didn't exist; removed at the
  end), English, light, Lord Default in `settings.json` (restored at the end).
- **Browsers:** throwaway profiles in `/tmp` (`chromium --user-data-dir=/tmp/csd-chr2`,
  `firefox-esr --profile /tmp/csd-ff --no-remote`), started from SSH with the session's environment
  (`WAYLAND_DISPLAY=wayland-0`, `XDG_SESSION_TYPE=wayland`, `GSETTINGS_BACKEND=keyfile`, the session
  bus). The user's own browser profiles were not touched. `master_preferences` was edited to test the
  new initial preferences on a new profile and put back.
- **Input and screenshots:** `lord-qmp-input.py 119 1280 800` and `lord-type.py 119` from the host,
  `screendump` holding `flock /tmp/sistema-119-screen.lock`. The "before" screenshot ran the saved
  October 18 install for a few minutes, then the new build again.
- **The migration** (`005-0.8.4-browser-frames.sh`) ran against copies under a scratch root
  (`LORDOS_MIGRATION_ROOT`): the 0.8.3 files change, a second run changes nothing, files an
  administrator edited stay.
- **Undone at the end:** `/usr/lib/lordos` restored from the backup (`diff -r` empty),
  `master_preferences` and `settings.json` restored, the themes, the schemas package, `session.conf`,
  the profiles, the tree, the script and the backup removed, `lordos.service` restarted to the login
  screen. No network or Proxmox setting was touched.

## Step 39 — 0.8.4: the browsers' title bar switch in Settings (VM 119 `lordos-spot`, October 19)

See `WAYLAND.md` ("The switch in Settings"). On VM 119 (172.17.0.54), tree in `~/LordOS-brs` (copied
with `tar` over SSH: the VM has no `rsync`), built there (`make -C server`, `make -C server test`)
and installed over `/usr/lib/lordos` by `~/deploy-brs-119.sh` (it saved `/usr/lib/lordos` to
`/var/backups/lordos-brs-bak` and `settings.json` to `/var/tmp` first).

- **Installed for the tests,** as in step 38: `gsettings-desktop-schemas` and `assets/gtk/LordOS*` in
  `/usr/share/themes/` (Chromium's GTK theme needs them for the traffic lights); both removed at the
  end.
- **Session:** `/etc/lordos/session.conf` with `AUTOLOGIN=lord` (it didn't exist; removed at the
  end), English and light in `settings.json` (restored at the end). A new session for the login-time
  tidy-up: the session's `lord-server` ended, `/run/lordos/autologin-done` removed, `systemctl
  restart lordos`.
- **Browsers,** started from SSH in the session (`WAYLAND_DISPLAY=wayland-0`, the session bus,
  `GSETTINGS_BACKEND=keyfile`) by `~/brs-run.sh`, quit with `SIGTERM` (Chromium) or Ctrl+Q and
  Return through `qm monitor 119` `sendkey` (Firefox: mouse clicks don't reach it on this VM).
  - **Chromium:** the user's `~/.config/chromium` was moved aside to `~/.config/chromium.brs-saved`
    and a throwaway profile made by starting Chromium; it was turned into a 0.8.3-style profile
    (`custom_chrome_frame` false) by hand, then: «Change them», the switch turned off while Chromium
    was open (applied seconds after it quit), and a check that Chromium keeps the edited values when
    it rewrites `Preferences`. Chromium left a stale `SingletonLock` after `SIGTERM`; it was
    correctly ignored. At the end the throwaway profile was deleted and the saved folder put back.
  - **Firefox:** `lord` already had two Firefox ESR profiles in `~/.config/mozilla/firefox` from
    October 16 (not checked before the first toggle, so the switch wrote to them): `user.js` lines
    and `user.js.lordos-backup` files, and Firefox ran three times on `eb7mmslg.default-esr` (its
    normal session files changed; `prefs.js` got `browser.tabs.inTitlebar`). At the end LordOS's
    files were removed from both profiles (`user.js`, `user.js.lordos-backup`; Firefox never creates a `user.js`, and both
    backups held only LordOS's line) and the `inTitlebar` line removed from
    `prefs.js` with Firefox closed, so it follows the default again.
- **Input and screenshots:** `lord-qmp-input.py 119 1280 800` from the host, `screendump` holding
  `flock /tmp/sistema-119-screen.lock`.
- **Undone at the end:** `/usr/lib/lordos` restored from the backup (the 90 paths the deploy added
  removed, `diff -r` against the backup empty), `settings.json` restored, `gsettings-desktop-schemas`
  purged, the GTK themes, `session.conf`, the scripts, the tree, the backup and `/tmp/brs-*` removed,
  `lordos.service` restarted to the login screen (the 0.8.3-era greeter kept running, as before).
  `eth0` keeps 172.17.0.54; no network or Proxmox setting was touched.

## Step 40 — 0.8.4: remote desktop over WebRTC (VM 122 `lordos-sandbox`, October 19)

See `REMOTE.md`, "Video (WebRTC)". On VM 122 (172.17.0.56), tree in `~/LordOS-rtc` (synced from the
Mac's `v084-rtc` worktree by a script of its own name, `sync-rtc-122.sh`, kept outside the repo),
built there (`make -C server`, `make -C server test`) and installed over the package's files by
`~/deploy-rtc-122.sh`, which saved `/usr/lib/lordos`, `/etc/lordos` and `/var/lib/lordos/remote` to
`/var/backups/lordos-rtc-20261019` first (`~/restore-rtc-122.sh` put them back). The theme bundles
were built on the Mac (the VM has no Node).

- **Installed for the build:** `libsrtp2-dev` and `libusrsctp-dev` (with `libsrtp2-1`, `libusrsctp2`,
  `libnss3-dev`, `libnspr4-dev`); removed at the end with `apt-get purge` + `autoremove`.
- **Installed for the TURN tests:** `coturn` (`--no-install-recommends`, with its 11 dependencies);
  its unit was stopped and disabled right after installing, and `turnserver` ran by hand on
  172.17.0.56:3478 with a test-only user from `/tmp/turn-rtc.conf`. Purged at the end.
- **Test-only accounts** `rtest` and `rtest2` (one password, only for these tests); `rtest` was put
  in `sudo` for the Settings → Sharing screenshots and the `remote-ice` action. Both deleted with
  their homes at the end.
- **The GPU path:** a drop-in `/etc/systemd/system/lordos.service.d/rtc-gpu-test.conf`
  (`Environment=LORD_GPU=1`, llvmpipe on this virtio card) and `rtest` signed in on the login screen
  through the Proxmox host's `lord-qmp-input.py` and `lord-type.py`; screenshots with `qm monitor 122`
  (`screendump`). The drop-in was removed.
- **Browsers:** headless Chrome on the Mac with throwaway profiles in the scratchpad (deleted after
  each run), driven over the DevTools protocol; CPU and traffic sampled on the VM by
  `~/cpu-rtc-122.sh` (`/proc/<pid>/stat`, `eth0` counters); the ICE/STUN probes by
  `/tmp/icetest-rtc.py` from the VM itself.
- **Configuration changed during the tests** (all restored from the backup): `config` (`net=`
  switched to `any` by a stray test click and back, `net=list` for one test, the TURN lines),
  `access` (one test `allow` entry), `turn.key`, `session.conf`.
- **Undone at the end:** `/usr/lib/lordos` and `/etc/lordos` restored from the backup (`diff -r`
  empty), `/var/lib/lordos/remote` restored, `/var/lib/lordos/last-user` back to `lord`, the
  packages above purged, the test accounts, the tree, the scripts, `/tmp` files and the backup
  removed, `lordos.service` restarted on the package's binaries, at the login screen. eth0 and the
  VM's network were not touched; VM 123 was not used.

## Step 41 — 0.8.4: the Pi kernel with socket destroy (VM 116 and the Raspberry Pi, October 19)

See `RASPBERRY.md` ("The LordOS kernel", "Switching kernels on a board") and `FIREWALL.md`
("Existing connections are cut").

- **VM 116 (172.17.0.52), cross build:** installed `gcc-aarch64-linux-gnu` and
  `libc6-dev-arm64-cross` (with their dependencies: `binutils-aarch64-linux-gnu`, the arm64 cross
  libraries) and the kernel build packages it lacked: `bison`, `flex`, `libelf-dev`, `libdw-dev`,
  `dwarves`, `rsync`, `debhelper`. They stay, for the next cross build. Work folder
  `/var/tmp/imager-scratch/pik-kernel` (the 24 GB scratch disk; about 16 GB at the peak) with a
  temporary 4 GB swap file; `RPI_KERNEL_REF=234188315037… tools/rpi/build-kernel.sh … 2712`, 67 min.
  The swap file and the work folder were removed afterwards; the `.debs` were copied out first.
- **Raspberry Pi 5 (172.17.0.24):** a first attempt to build natively (in a tmpfs, the SD card being
  86 % full) ended when the card was removed; nothing of it is left. Then:
  - `/var/tmp/lordos-kernel/out/`: the `-3` image and headers `.debs` and their config added
    (`SHA256SUMS` regenerated), where `build-img.sh` takes them from.
  - `dpkg -i linux-image-6.18.57-lordos.3-rpi-2712_6.18.57-3+lordos_arm64.deb`: the hooks put it at
    the top level of `/boot/firmware` (over Raspberry Pi's 6.18.50 copy there, which is still
    installed; the October 18 backup is in `/var/tmp/lordos-kernel/bootbak-20261018`). `-2` stays
    installed.
  - `/boot/firmware/lordos/` (the `-2` boot files) renamed `lordos-prev/`; `config.txt` lost its
    `os_prefix=lordos/` lines (a comment explains how to go back); `tryboot.txt` now boots
    `lordos-prev/` once. Before the change: `/var/tmp/lordos-pik/config.txt.before` and
    `tryboot.txt.before`. First boot of `-3` through `tryboot`, then a normal boot, a `tryboot` into
    `-2` (69 modules loaded, everything active) and back.
  - `lord-netperm` was still the 0.8.2 binary (not restarted by the 0.8.3 upgrade): restarted.
  - Tests in `/var/tmp/lordos-pik` (tree copy in `src/`, logs): `netperm_cut.sh` as root with a
    hand-made `lordos-session-0` cgroup (gone at the reboot; `/root/.config/lordos` removed), before
    (12/15, the stall) and after (15/15); `verify-board`; `lord-fand` `FORCE 1`/`FORCE off`;
    `make -C server test`.
  - Not changed: the network configuration, the `lordbasex` account, the partitions, `cmdline.txt`
    (this board's lacks `cgroup_enable=memory`, so `verify-board` reports the memory controller as
    FAIL; the image's has it).

## Step 42 — 0.8.4: LordOS Imager (VM 116 `lordos-gpu`, October 19)

See `IMAGER.md`. Tree in `~/LordOS-imager`, built there (`make -C server`, `make -C server test`:
23 passed, the imager suites included) and installed over `/usr/local/lib/lordos` by
`~/deploy-imager-116.sh`, which first saved `/usr/local/lib/lordos` and lord's `settings.json` in
`/var/backups/lordos-imager-bak`.

- **A scratch disk:** `qm set 116 --scsi1 local-lvm:24,discard=on` on the Proxmox host (as VM 119's
  `sata1` before it), formatted ext4 (label `imgscratch`) and mounted at `/var/tmp/imager-scratch`
  (not in fstab); `~/ImagerTest` was a bind mount of its `user/` folder, so test images sat in the
  home folder as the imager requires. It also served as the internal, non-removable disk the
  imager must refuse.
- **A virtual USB stick:** `modprobe dummy_hcd` and `modprobe g_mass_storage
  file=/var/tmp/imager-scratch/usb-stick-116.img removable=1` (4 GB, then 16 GB, sparse): `sdc`,
  `TRAN usb`, `RM 1`. Every write was preceded by `~/ImagerTest/vguard.sh sdc`, which prints the
  device and stops unless it is that gadget with the backing file's size. No other disk was written.
- **Images:** the signed 0.8.1 release folder copied from the development machine, and 0.8.3
  downloaded from GitHub by `imager-fetch` into `~/.cache/lordos/imager`.
- **Session:** `/etc/lordos/session.conf` with `AUTOLOGIN=lord` (it didn't exist) and lord's
  `settings.json` set to English, both put back at the end; the locked `pwtest` session from an earlier
  test was ended by the service restarts.
- **The Linux download (Tauri) inside the LordOS session:** installed for the test only
  `libwebkit2gtk-4.1-0` and `udisks2` with their dependencies (61 packages, listed in
  `~/ImagerTest/linux-test-packages.txt`), and `/etc/polkit-1/rules.d/49-lordos-imager-test.rules`
  allowing `lord` the udisks2 open/mount/eject actions (the LordOS session has no polkit agent to
  ask); the app ran as a Wayland client with `WAYLAND_DISPLAY=wayland-0`. All of it was removed at
  the end (`apt-get purge` of those packages, then the rule).
- **Screenshots** with `qm monitor 116` (`screendump`) from the Proxmox host, the pointer with
  `/root/lord-qmp-input.py 116` and keys with `/root/lord-type.py 116`.
- **Undone at the end:** `/usr/local/lib/lordos` restored from the backup, `session.conf` removed and
  lord's `settings.json` restored, `lordos.service` restarted to the login screen; the gadget
  unloaded (`rmmod g_mass_storage dummy_hcd`), the bind mount and the scratch mount removed, and the
  scratch disk detached and deleted from the VM (`qm set 116 --delete scsi1`, then the unused volume
  removed); `~/ImagerTest`, `~/.cache/lordos/imager`, `~/.config/LordOS Imager` and the test key and
  image files deleted. `~/LordOS-imager` (the tree) is kept, like the other agents' trees. No network
  or Proxmox setting other than the scratch disk was touched.

## Step 43 — 0.8.4: the remote desktop WebRTC only (VM 122 `lordos-sandbox`, October 19)

See `REMOTE.md`, "Video (WebRTC)", "ICE over TCP", "Reconnecting" and "When it can't connect". On
VM 122 (172.17.0.56), tree in `~/LordOS-rtc2` (copied from the Mac's `v084-rtc-only` worktree with
`tar` over SSH: the VM has no `rsync`), built there (`make -C server`, `make -C server test`) and
installed over the package's files by `~/deploy-rtc2-122.sh`, which needed the backup of
`/usr/lib/lordos`, `/etc/lordos`, `/var/lib/lordos/remote`, `/var/lib/lordos/last-user`, the
`iptables-save` output and the package selections in `/var/backups/lordos-rtc2-20261019` first
(`~/restore-rtc2-122.sh` put them back). The theme bundles were built on the Mac (the VM has no Node).
For the before/after measurements, `main`'s `server/` was built in `~/LordOS-rtc2-base` and its three
binaries installed for one round, then this branch's again.

- **Installed for the build:** `libsrtp2-dev` and `libusrsctp-dev` (with `libsrtp2-1`, `libusrsctp2`,
  `libnss3-dev`, `libnspr4-dev`); purged at the end.
- **Installed for the TURN tests:** `coturn` (`--no-install-recommends`, with `inetutils-telnet`,
  `telnet`, `libevent-extra-2.1-7t64`, `libevent-openssl-2.1-7t64`, `libevent-pthreads-2.1-7t64`,
  `libhiredis1.1.0`, `libmariadb3`, `libpq5`, `mariadb-common`, `mysql-common`, `sqlite3`); its unit
  was disabled and stopped right after installing, and `turnserver` ran by hand on 172.17.0.56 (3478
  UDP/TCP, 5349 TLS with a throwaway self-signed P-256 certificate, relay ports 49160–49200) with a
  test-only user from `/tmp/turn-rtc2/turn.conf`. Purged at the end; `/tmp/turn-rtc2` removed.
- **Firewall rules for the tests only:** `iptables` (the nftables backend, already installed) on the
  VM's INPUT chain, only for the Mac's address (172.17.0.5): drop UDP; drop TCP 5673; drop new TCP
  connections to 5673 beyond the first (`connlimit`, so the WebSocket stays and ICE-TCP can't); drop
  UDP except the relay ports. Each rule was deleted right after its test; eth0 and the VM's network
  configuration were never touched. The rules left an empty `ip filter` table behind, removed with
  `nft delete table ip filter` (`nftables` installed for that one command and purged), so
  `iptables-save` is empty again, as before.
- **Test-only accounts** `rtest` and `rtest2` (one password, only for these tests); `rtest` was put
  in `sudo` for Settings → Sharing → Connection (the `remote-ice` action through the UI) and its
  screenshots. Both deleted with their homes at the end (their `/run/lordos/spawn-<uid>.lock` too).
- **Browsers:** headless Chrome on the Mac with throwaway profiles in the scratchpad, driven over the
  DevTools protocol; the ICE and ICE-TCP probes (`/tmp/iceprobe-rtc2.py`) ran from the VM itself as a
  second address; CPU and traffic from `/proc` and `eth0` (`/tmp/cpu-rtc2.py`). The fuzzer's 1,000,000
  input run: `./build/remote-fuzz 1000000 7` (2 min 29 s).
- **Configuration changed during the tests** (all restored from the backup): `config` (`turn*` lines
  written by hand and by Settings, `net=list` for one test, `net=lan` written by Settings), `access`
  (one test `allow` line), `turn.key`. One `systemctl kill -s HUP lordos` signalled the whole unit
  instead of only `lord-sessiond`, so `lord-netd` restarted once ("the network was not interrupted");
  use `systemctl reload lordos` (`ExecReload` sends SIGHUP to the main process only).
- **Undone at the end:** `/usr/lib/lordos`, `/etc/lordos`, `/var/lib/lordos/remote` and `last-user`
  restored (`diff -r` empty), the packages above purged (`dpkg --get-selections` identical to before),
  `iptables-save` as before, the test accounts, both trees, the scripts, the `/tmp` files and the
  backup removed, `lordos.service` restarted on the package's binaries, at the login screen.
  VMs 115, 106 and 123 were not used.

## Step 44 — 0.8.4 release test (VMs 116, 117, 120 and the Raspberry Pi, October 19–25)

See `ISO.md`, "Release test (0.8.4 Beta, October 19–25)".

- **VM 116:** installed `libsrtp2-dev` and `libusrsctp-dev` (with `libsrtp2-1`, `libusrsctp2`,
  `libnss3-dev`, `libnspr4-dev`), which `lord-sessiond` now needs; they stay, for the next release
  builds. The release tree in `~/LordOS-iso`, built there; ISO and `.deb` in
  `/var/tmp/lordos-iso/out/` (the 0.8.2 and 0.8.3 ISOs there were removed to make room: identical
  copies are in Proxmox's `local:iso`), `apt-get clean` run. Left stopped, as it was found on the
  25th.
- **Proxmox:** `local:iso/lordos-live-0.8.4-amd64.iso` added. A temporary typing helper in the
  host's `/root` was removed afterwards.
- **VMs 117 and 120:** a Proxmox snapshot `r084pre` first; then 120 got a fresh 0.8.3 install from
  the 0.8.3 ISO (test user `rtester`, `openssh-server`, a sudoers drop-in, `lordos.sources` on
  `apt-staging`, xterm, a Chromium profile opened once) and a snapshot `r084base`, and was upgraded
  three times (Store with the first build; apt and Store again with the final build, back at
  `r084base` each time; for the tests also `smartmontools` and `iptables`); 117 installed the 0.8.4
  ISO. Both were rolled back to `r084pre` (ISO, boot order and disks as before: the earlier "Test
  User" installs, at the login screen), the snapshots deleted, and both left stopped.
- **Raspberry Pi 5 (172.17.0.24):** installed `libsrtp2-dev` and `libusrsctp-dev` (with `libsrtp2-1`,
  `libusrsctp2`, `libnss3-dev`, `libnspr4-dev`) for the native build. Removed the old image work
  folders `/var/tmp/lordos-rpi-v081` (an unpublished 0.8.1 build) and `-v082b` (its image is the
  published 0.8.2 one, same SHA-256) to make room. Built the arm64 package and the image in
  `/var/tmp/lordos-src-084` and `/var/tmp/lordos-rpi-v084c`. The installed LordOS went from 0.8.3 to
  0.8.4 with apt (from staging, then the source put back on the live repository), migration 005;
  `lordos`, `lord-netperm` and `lord-fand` restarted, no reboot. The firewall cut test ran as root
  with a hand-made `lordos-session-0` cgroup (removed afterwards, with the empty
  `/root/.config/lordos` it created); the first-run check used a loop device on a decompressed copy
  of the image in `/var/tmp/r084-fr` (removed). Network, the `lordbasex` account and the SD
  partitions untouched; the board was not reflashed.

## Step 45 — 0.8.4.1 release test (VMs 116 and 120 and the Raspberry Pi, October 25)

See `ISO.md`, "Release test (0.8.4.1 Beta, October 25)".

- **VM 116:** started (it was stopped); the release tree in `~/LordOS-0841`, built and tested there;
  the `.deb` and its SBOM in `/var/tmp/`. Nothing installed. Left stopped.
- **VM 120:** a Proxmox snapshot `r0841pre` first; then a fresh 0.8.4 install from the 0.8.4 ISO
  (test user `rtester`, `openssh-server`, a sudoers drop-in, `lordos.sources` on `apt-staging`) and
  a snapshot `r0841base`; upgraded with the Store, then again with apt from `r0841base`. Rolled back
  to `r0841pre` (the earlier "Test User" install, at the login screen; ISO and boot order as
  before), both snapshots deleted, left stopped.
- **Raspberry Pi 5 (172.17.0.24, the user's TEST SD with the 0.8.4 image):** clock set by hand
  (the image had no time client), `debootstrap`, `xz-utils`, `curl`, `gpg`, `dosfstools`, `fdisk`,
  `dpkg-dev`, `python3`, `dpkg-repack` and `file` installed for the native image build; the
  installed LordOS kernels repacked with `dpkg-repack` into `/var/tmp/lordos-kernel/out` (`dpkg
  --verify` clean first: the same files as the 0.8.4 image's). Tree in `/var/tmp/lordos-src-0841`,
  image work folder `/var/tmp/lordos-rpi-v0841`. `lordos` 0.8.4 → 0.8.4.1 from staging (then the
  source put back on the live repository). For the Settings test: a test-only account `rtest`
  (sudo group) and the remote desktop turned on; both removed afterwards. Bootloader updated once
  through Settings → Board firmware (2026/05/26 → 2026/09/25). Chromium and Firefox ESR installed
  from Debian with the image's browser defaults, to check them (left installed). Network and
  partitions untouched; the board was not reflashed.

## Step 46 — Monitor → Network, second generation, for 0.8.5 (VM 118 `lordos-notes`, October 19)

See `MONITOR.md` ("Network, second generation", "The menu bar network item", "Blocklists") and
`FIREWALL.md` ("LordOS components in their own scopes", "The essential system services",
"Blocklists"). On VM 118 (172.17.0.53), tree in `~/LordOS-netmon` (copied over SSH with `tar`: the
VM has no rsync), built there (`make -C server`, `make -C server test`, `sudo build/npverify-test`)
and installed over `/usr/local/lib/lordos` (`lord-server`, `lord-netperm`, the new `lord-fetch`,
themes and assets) by `~/deploy-netmon-118.sh`, which first saved `/usr/local/lib/lordos` in
`/var/backups/lordos-netmon-bak` and `~/.config/lordos` in `~/.config-lordos-backup-netmon`.
`lord-sessiond` was not replaced (lord-netd isn't installed on this VM, so the network service row
was not tested here). Nothing was installed with apt.

- **Unit drop-in:** `/etc/systemd/system/lord-netperm.service.d/netmon084.conf` with
  `RestrictAddressFamilies=AF_UNIX AF_PACKET AF_NETLINK` (this VM's unit predates 0.8.3's sock_diag);
  removed at the end.
- **Session:** the autologin session of `lord` (`/run/lordos/autologin-done` removed, the session's
  lord-server ended, `lordos` restarted), English, dark and then light for the screenshots, the menu
  bar item on (all in `settings.json`, restored at the end).
- **Test traffic:** `curl` started from SSH inside `lordos-net-terminal.scope` (as a Terminal program)
  and, for the A/B measurements, inside `app-lordos-npbench.scope`: HTTPS requests to public sites
  every 4 s, two rate-limited downloads from Debian mirrors, and attempts to a listed scam domain
  (denied by the list; while the lists were off for the A/B runs, a few requests to it went through).
  `nc -z` to a listed domain checked that the owner's allow rule wins. The Store's "Check for
  updates" for the Store's own row.
- **Blocklists:** four catalog lists and one custom list downloaded over HTTPS from their sources;
  copies in `~/.cache/lordos/fwlists`, removed at the end with `fwlists.json` and `fwlists.txt`.
- **Measurements:** `iperf3` (already on this VM since step 28) through a veth pair `npb0`/`npb1`
  into a private namespace `npb` (10.213.7.0/30), deleted after each run; eth0 and the network
  configuration were not touched. For the A/B runs the 0.8.3 `lord-netperm` and main's build
  (built in `~/LordOS-netmon-base`) were swapped in for a few minutes and the new build put back.
- **Input and screenshots:** `lord-qmp-input.py 118 2987 1067` and `lord-type.py 118` from the
  Proxmox host, `screendump` converted to PNG on the development machine.
- **Undone at the end:** `/usr/local/lib/lordos` restored from the backup (`diff -r` empty), the
  drop-in removed and `lord-netperm` restarted with the old binary, `~/.config/lordos` restored
  (`diff -r` against the backup empty), the blocklist files and the test scopes removed, the trees,
  scripts and backups deleted, `lordos` restarted to the login screen. The traffic history
  (`~/.local/state/lordos/net-history.bin`) keeps the test minutes (app ids and byte counts only).
  No Proxmox setting was touched.

## Step 47 — 0.8.5: System Information's collector (VM 116 `lordos-gpu`, October 26)

See `BENCHMARK.md` ("0.8.5 — System Information"). Built in `~/LordOS-085fs` (`make -C server
build/lordos-sysreport build/sysreport-test`) and run from there as `lord`; nothing was installed
over `/usr/local/lib/lordos`.

- **Installed:** `usb.ids` (Debian, architecture `all`: the USB vendor and product names, read by
  `lordos-sysreport`; `pci.ids` was already installed). The package recommends both.
- **The app:** `lord-server` from that tree in an isolated headless session (`--headless 1280x800`,
  its own `HOME`, `XDG_RUNTIME_DIR` and `dbus-run-session`, removed afterwards), driven and captured
  over its own `remote.sock` (keys, clicks and the still frame's lossless tiles); the user's session
  was not touched.
- **Installed build (with the user's go-ahead):** `/usr/local/lib/lordos` replaced by the 0.8.5 test build
  (backup of the previous one in `/usr/local/lib/lordos.1026.bak`), `lordos.service` restarted;
  `lordos-sysreport`, `lordos-fetch` and `lordos-testbench` in `/usr/local/bin`, and
  `/usr/share/applications/lordos-testbench.desktop`.
- **lord-netperm (with the user's go-ahead to change the VM):** VM 116 had no per-app firewall service (System
  Information → Checks reported it). Installed `/usr/local/lib/lordos/bin/lord-netperm` and
  `/etc/systemd/system/lord-netperm.service` (the ISO's unit with that path), enabled and started; the network
  kept working.

## Step — 0.8.5 release: Raspberry Pi kernel and image on VM 116 (2026-10-27)

To cross-build the LordOS Pi 5 kernel (`tools/rpi/build-kernel.sh`, pinned commit
`234188315037848e5b07d65b85d38edc64095caa`) and the Pi image (`tools/rpi/build-img.sh`) on the x86-64
VM 116, as `docs/RASPBERRY.md` lists:

```sh
sudo apt-get install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross build-essential \
  bc bison flex libssl-dev libelf-dev libdw-dev dwarves cpio kmod rsync dpkg-dev debhelper git \
  qemu-user-static binfmt-support fdisk dosfstools e2fsprogs xz-utils curl gpg util-linux
```

## Step — 0.8.6 in development: LordBench on VM 116 (2026-10-27)

See `BENCHMARK.md` ("Built so far"). Built in `~/LordOS-086` (`make -C server`, plus `build/lordbench-cli` to run
it from a terminal) and run from there as `lord`; nothing was installed over `/usr/local/lib/lordos` and no
package was added. The build uses `xz` (xz-utils, already installed: `dpkg-deb` depends on it) to compress the
xz workload's fixed text (`build/gen/lb_corpus.h`). The app was checked in an isolated headless session of that
tree (own `HOME`, `XDG_RUNTIME_DIR` and bus, driven over its `remote.sock`), removed afterwards.

## Step — 0.8.7 in development: the arm64 live ISO on VM 116 (2026-10-30)

`tools/iso/build-arm64.sh` and booting the result under emulation (`docs/ISO.md`, "The arm64 ISO"):

```sh
sudo apt-get install -y --no-install-recommends qemu-system-arm qemu-efi-aarch64
```

`qemu-system-x86` was also installed on VM 116 earlier and was not recorded then:
`sudo apt-get install -y --no-install-recommends qemu-system-x86`. The packages `build-arm64.sh` adds (debootstrap,
squashfs-tools, xorriso, grub-common, grub-efi-arm64-bin, mtools, dosfstools) go into an overlay over the arm64 chroot,
not on the VM and not into the chroot itself.
