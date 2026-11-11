#!/bin/sh
# mkdeb.sh — builds lordos_<version>_<arch>.deb from the already compiled tree.
# Usage: tools/iso/mkdeb.sh <output.deb>     (from the repository root; needs dpkg-deb)
#
# Contents:
#   /usr/lib/lordos/bin/lord-sessiond     sessions, login screen and remote desktop (root)
#   /usr/lib/lordos/bin/lord-netperm      per-app network permissions with cgroup BPF (root, own service)
#   /usr/lib/lordos/bin/lord-netd         system networking (root; launched and watched by lord-sessiond)
#   /usr/lib/lordos/bin/lord-fand         fan control by temperature (root, own service, enabled)
#   /usr/lib/lordos/bin/lord-autoinstall  the live ISO's unattended install (root, lordos-autoinstall.service; docs/UNATTENDED.md)
#   /usr/lib/lordos/bin/lord-pkexec       /usr/bin/pkexec on LordOS, no privilege (docs/PRIVACY.md); the real one
#                                         is diverted to /usr/bin/pkexec.distrib by postinst
#   /usr/lib/lordos/bin/lord-server       each session's server (looks for ../themes and ../assets)
#   /usr/bin/lordos-sysreport             System Information's report and LordBench as a command (user;
#                                         docs/BENCHMARK.md): a link to /usr/lib/lordos/bin/lordos-sysreport,
#                                         which starts the lord-bench and lord-media next to it
#   /usr/bin/lordos-fetch                 the crown and a summary of the computer (a link to the same binary)
#   /usr/bin/lordos-issue                 what a bug report needs, ready to paste (CONTRIBUTING.md)
#   /usr/bin/lordos-testbench             the visual test bench (a Wayland client; docs/PAPER-VALIDATION.md), and
#   /usr/share/applications/lordos-testbench.desktop   its launcher entry
#   /usr/lib/lordos/themes/<theme>/       theme.json, theme.bundle.js and their resources (greeter: the
#                                         login screen)
#   /usr/lib/lordos/assets/               fonts, texts (locales), icons, shell integration
#   /usr/lib/lordos/code-manifest.json    SHA-256 of every bundle LordOS runs as its own code, and
#   /usr/lib/lordos/code-manifest.sig     its Ed25519 signature (release key; docs/SIGNING.md)
#   /usr/lib/lordos/lordos-live-prep      live preparation (grub language, folders)
#   /usr/lib/systemd/system/lordos.service  lord-sessiond on tty1
#   /usr/lib/lordos/lordos-splash         boot splash hand-off (Plymouth → LordOS, docs/SESSIONS.md)
#   /usr/share/backgrounds/lordos/        the wallpapers (<category>/lordos-<name>.jpg, thumbs/), linked
#                                         from /usr/lib/lordos/assets/wallpapers
#   /usr/share/plymouth/themes/lordos/    the boot splash theme (selected by the ISO, the Raspberry Pi
#                                         image or install.sh; the package never selects it)
#   /usr/share/grub/themes/lordos/        the boot menu theme (the ISO's disk installer and install.sh; unused on a Pi)
#   /usr/share/doc/lordos/                README, copyright, changelog, OFL-Noto.txt
#   /usr/share/glib-2.0/schemas/90_lordos.gschema.override, /etc/gtk-{3,4}.0/settings.ini
#                                         GTK's window-button layout where the session can't give its own (#52)
#   (not here: /usr/lib/lordos/assets/hwreport/, the signed tested-hardware snapshot, is the lordos-hwdata
#   package from tools/iso/mkdeb-hwdata.sh, which this one Depends on; docs/BENCHMARK.md)
#   /usr/lib/lordos/apt/lordos.sources    the signed LordOS repository on lordos.org (updates from the
#                                         Store and apt; docs/UPDATES.md): postinst copies it to
#                                         /etc/apt/sources.list.d/ when missing, postrm removes it
#   /usr/share/keyrings/lordos-archive-keyring.gpg   the key that signs that repository
#   /usr/lib/lordos/migrate, migrations/  version-to-version system migrations, run by postinst
#   /etc/kernel/postinst.d/zz-lordos-reboot-required  "restart required" after a kernel update
#   /usr/lib/modules-load.d/lordos-drivetemp.conf    SATA disk temperatures (drivetemp) at boot
#   /usr/lib/systemd/system/smartmontools.service.d/lordos.conf  Debian's smartd stays off
#   /usr/lib/systemd/user/gnome-keyring-daemon.service.d/lordos.conf  gnome-keyring without the Secret Service
#   /usr/lib/systemd/logind.conf.d/lordos-machines.conf  a shutdown waits up to 30 s for Machines to save running machines
#   /usr/share/chromium/extensions/lordos-video/  LordOS's own browser extension for smooth video without
#                                         hardware decoding (Debian's chromium loads every folder there;
#                                         docs/VIDEO.md). Its settings are written at login by the
#                                         admin action video-optimize, not shipped (postrm removes them)
set -eu
OUT=${1:?uso: mkdeb.sh salida.deb}
ROOT=$(pwd)
VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' package.json | head -1)
[ -x server/build/lord-server ] || { echo "missing server/build/lord-server (make -C server)" >&2; exit 1; }
[ -x server/build/lord-netperm ] || { echo "missing server/build/lord-netperm (make -C server; clang for BPF)" >&2; exit 1; }
[ -x server/build/lord-netd ] || { echo "missing server/build/lord-netd (make -C server)" >&2; exit 1; }
[ -x server/build/lord-fand ] || { echo "missing server/build/lord-fand (make -C server)" >&2; exit 1; }
[ -x server/build/lordos-sysreport ] || { echo "missing server/build/lordos-sysreport (make -C server)" >&2; exit 1; }
[ -x server/build/lord-sessiond ] || { echo "missing server/build/lord-sessiond (make -C server, with libssl-dev, libsrtp2-dev and libusrsctp-dev)" >&2; exit 1; }
# The helpers make builds only when their -dev packages are installed (docs/VM-SETUP.md). A package without one of
# them loses a feature with no error anywhere (0.8.8.6 shipped without lord-media: Screenshots saved and copied
# nothing), so each one is required here. ALLOW_MISSING="lord-tts …" leaves out the ones named, on purpose.
missing=""
for h in lord-media:"libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libva-dev" \
         lord-doc:"libpoppler-glib-dev libarchive-dev" lord-ocr:"libtesseract-dev" lord-tts:"tools/tts/fetch-audiocpp.sh" \
         lord-vmdisplay:"libsystemd-dev" lord-vault:"" lord-maild:"" lord-bench:"" lord-clean:"" lord-lan:"" lord-fetch:"" \
         lord-tls:"" lord-sql:"" lord-sftp:"" lord-vm:"" lordos-guest:""; do
    b=${h%%:*}; need=${h#*:}
    [ -x "server/build/$b" ] && continue
    case " ${ALLOW_MISSING:-} " in *" $b "*) echo "mkdeb: leaving out $b (ALLOW_MISSING)" >&2; continue ;; esac
    missing="$missing\n  server/build/$b${need:+  (needs $need)}"
done
[ -z "$missing" ] || { printf "missing helpers, the package would ship without them:$missing\nmake -C server after installing what they need, or ALLOW_MISSING=\"name …\" to leave one out on purpose\n" >&2; exit 1; }
[ -f themes/greeter/theme.bundle.js ] || { echo "missing the login screen (npm run build)" >&2; exit 1; }
for t in themes/*/theme.bundle.js; do [ -f "$t" ] || { echo "built themes missing (npm run build)" >&2; exit 1; }; done

PKG=$(mktemp -d)
trap 'rm -rf "$PKG"' EXIT
L="$PKG/usr/lib/lordos"
install -d "$L/bin" "$L/themes" "$L/assets" "$PKG/usr/lib/systemd/system" "$PKG/usr/share/doc/lordos" "$PKG/DEBIAN"
install -m 755 server/build/lord-server "$L/bin/lord-server"
install -m 755 server/build/lord-sessiond "$L/bin/lord-sessiond"
install -m 755 server/build/lord-netd "$L/bin/lord-netd"
install -m 755 server/build/lord-netperm "$L/bin/lord-netperm"
install -m 755 server/build/lord-fand "$L/bin/lord-fand"
# /usr/bin/pkexec on LordOS: postinst diverts the real one and links this (docs/PRIVACY.md, "Authentication for Linux apps")
install -m 755 server/build/lord-pkexec "$L/bin/lord-pkexec"
# the live ISO's unattended install (docs/UNATTENDED.md); its unit is enabled only on the ISO (tools/iso/build.sh)
install -m 755 server/build/lord-autoinstall "$L/bin/lord-autoinstall"
# strip for the package's architecture: the host's strip can't read another architecture's binaries
# (an aarch64 server/build packaged on x86-64 by tools/rpi/build-img.sh), and its errors are ignored below
STRIP=strip
if [ "${DEB_ARCH:-}" = arm64 ] && [ "$(dpkg --print-architecture)" != arm64 ]; then
    STRIP=aarch64-linux-gnu-strip
    command -v "$STRIP" > /dev/null || { echo "missing $STRIP (binutils-aarch64-linux-gnu)" >&2; exit 1; }
fi
# the LordOS Imager's first-run file applier (Raspberry Pi image, rpi/firstboot; docs/IMAGER.md)
install -m 755 server/build/lordos-firstrun "$L/bin/lordos-firstrun"
# System Information's report and LordBench for a terminal, SSH or CI; lord-server never runs it. In the
# LordOS folder because `bench` only starts the lord-bench next to its own binary (/proc/self/exe)
install -d "$PKG/usr/bin"
install -m 755 server/build/lordos-sysreport "$L/bin/lordos-sysreport"
"$STRIP" --strip-unneeded "$L/bin/lordos-sysreport" 2>/dev/null || true
ln -s ../lib/lordos/bin/lordos-sysreport "$PKG/usr/bin/lordos-sysreport"
ln -s ../lib/lordos/bin/lordos-sysreport "$PKG/usr/bin/lordos-fetch"
install -m 755 packaging/bin/lordos-issue "$PKG/usr/bin/lordos-issue"
# the visual test bench: a plain Wayland client, launched like any Linux app
[ -x server/build/lordos-testbench ] && {
    install -m 755 server/build/lordos-testbench "$PKG/usr/bin/lordos-testbench"
    "$STRIP" --strip-unneeded "$PKG/usr/bin/lordos-testbench" 2>/dev/null || true
    install -d "$PKG/usr/share/applications"
    install -m 644 packaging/desktop/lordos-testbench.desktop "$PKG/usr/share/applications/lordos-testbench.desktop"
}
# isolated PDF and archive helper (optional: make skips it without libpoppler-glib-dev/libarchive-dev);
# lord-server only launches the lord-doc in its own folder
[ -x server/build/lord-doc ] && install -m 755 server/build/lord-doc "$L/bin/lord-doc"
# Claude Code's status line bridge for the Terminal's status bar (docs/TERMINAL.md); Claude Code runs it, not lord-server
install -m 755 server/build/lord-claude-status "$L/bin/lord-claude-status"
# text to speech (docs/VOICE.md): `say` (lord-say, no libraries) and its sandboxed engine lord-tts, the only one that
# links audio.cpp/ggml, with those libraries in lib/tts (ggml loads its CPU and Vulkan backends from there). Optional:
# make skips lord-tts without tools/tts/fetch-audiocpp.sh's source; no voice model is installed (Settings -> AI -> Voice)
install -m 755 server/build/lord-say "$L/bin/lord-say"
if [ -x server/build/lord-tts ]; then
    install -m 755 server/build/lord-tts "$L/bin/lord-tts"
    mkdir -p "$L/lib/tts"
    cp -P server/build/lib/tts/*.so* "$L/lib/tts/"
    for f in "$L"/lib/tts/*.so*; do [ -L "$f" ] || { chmod 644 "$f"; "$STRIP" --strip-unneeded "$f" 2>/dev/null || true; }; done
    # audio.cpp's description of Kyutai's HD model folders (server/mk/tts.mk; lord-tts reads it, docs/VOICE.md)
    install -m 644 server/build/lib/tts/pocket_tts_dir.json "$L/lib/tts/pocket_tts_dir.json"
fi
# isolated text recognition helper, Screenshots' "copy the text" (optional: make skips it without libtesseract-dev)
[ -x server/build/lord-ocr ] && install -m 755 server/build/lord-ocr "$L/bin/lord-ocr"
"$STRIP" --strip-unneeded "$L/bin/lord-server" "$L/bin/lord-sessiond" "$L/bin/lord-netd" "$L/bin/lord-netperm" "$L/bin/lord-fand" "$L/bin/lord-pkexec" "$L/bin/lord-autoinstall" "$L/bin/lordos-firstrun" 2>/dev/null || true
[ -x "$L/bin/lord-doc" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-doc" 2>/dev/null || true; }
[ -x "$L/bin/lord-ocr" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-ocr" 2>/dev/null || true; }
"$STRIP" --strip-unneeded "$L/bin/lord-claude-status" "$L/bin/lord-say" 2>/dev/null || true
[ -x "$L/bin/lord-tts" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-tts" 2>/dev/null || true; }
# lord-server only launches the lord-media in its own folder (media: video, photos, camera, recorder)
[ -x server/build/lord-media ] && install -m 755 server/build/lord-media "$L/bin/lord-media"
[ -x "$L/bin/lord-media" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-media" 2>/dev/null || true; }
# lord-vault: Passwords and the system keyring (docs/PASSWORDS.md); lord-server only launches the one in its own folder
[ -x server/build/lord-vault ] && install -m 755 server/build/lord-vault "$L/bin/lord-vault"
[ -x "$L/bin/lord-vault" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-vault" 2>/dev/null || true; }
# lord-maild: IMAP/SMTP/CalDAV/CardDAV with TLS for Mail (docs/MAIL.md); lord-server only launches the one in its own folder
[ -x server/build/lord-maild ] && install -m 755 server/build/lord-maild "$L/bin/lord-maild"
[ -x "$L/bin/lord-maild" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-maild" 2>/dev/null || true; }
# lord-bench (0.8.6): LordBench's sandboxed workloads (docs/BENCHMARK.md); lord-server only launches the one in its own folder
[ -x server/build/lord-bench ] && install -m 755 server/build/lord-bench "$L/bin/lord-bench"
[ -x "$L/bin/lord-bench" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-bench" 2>/dev/null || true; }
# lord-clean (0.8.7): Cleaner's sandboxed read-only scanner (docs/CLEANER-ARCHITECTURE.md); lord-server only launches the one in its own folder
[ -x server/build/lord-clean ] && install -m 755 server/build/lord-clean "$L/bin/lord-clean"
[ -x "$L/bin/lord-clean" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-clean" 2>/dev/null || true; }
# lord-lan (0.8.7): reads what the local network answers for Files → Network, sandboxed (docs/FILES.md, "Network")
[ -x server/build/lord-lan ] && install -m 755 server/build/lord-lan "$L/bin/lord-lan"
[ -x "$L/bin/lord-lan" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-lan" 2>/dev/null || true; }
# lord-rdesk (0.8.7): one RDP or VNC connection, sandboxed (docs/FILES.md, "Remote desktop")
install -m 755 server/build/lord-rdesk "$L/bin/lord-rdesk"
"$STRIP" --strip-unneeded "$L/bin/lord-rdesk" 2>/dev/null || true
# Machines (0.8.9, alpha internals): lord-vm supervises one machine, lord-vmdisplay carries its picture and input
# (docs/VIRTUALIZATION.md, "Phase 1 implementation"). QEMU and passt aren't dependencies: Machines asks for them.
for b in lord-vm lord-vmdisplay; do
    [ -x "server/build/$b" ] && install -m 755 "server/build/$b" "$L/bin/$b"
    [ -x "$L/bin/$b" ] && { "$STRIP" --strip-unneeded "$L/bin/$b" 2>/dev/null || true; }
done
# lordos-guest (0.8.9 beta): the clipboard helper Machines copies into a guest of this architecture (Install Agent)
if [ -x server/build/lordos-guest ]; then
    install -d "$L/guest"
    install -m 755 server/build/lordos-guest "$L/guest/lordos-guest-${DEB_ARCH:-$(dpkg --print-architecture)}"
    "$STRIP" --strip-unneeded "$L/guest/lordos-guest-${DEB_ARCH:-$(dpkg --print-architecture)}" 2>/dev/null || true
fi
# lord-sql (0.8.8.7): the Databases app's MySQL / MariaDB client, sandboxed (docs/DATABASES.md)
[ -x server/build/lord-sql ] && install -m 755 server/build/lord-sql "$L/bin/lord-sql"
[ -x "$L/bin/lord-sql" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-sql" 2>/dev/null || true; }
# lord-sftp (0.8.8.7): the Terminal's Files mode, SFTP over the tab's own ssh session, sandboxed (docs/TERMINAL.md)
[ -x server/build/lord-sftp ] && install -m 755 server/build/lord-sftp "$L/bin/lord-sftp"
[ -x "$L/bin/lord-sftp" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-sftp" 2>/dev/null || true; }
# lord-fetch (0.8.5): downloads the subscribed blocklists over HTTPS, sandboxed (docs/FIREWALL.md, "Blocklists")
[ -x server/build/lord-fetch ] && install -m 755 server/build/lord-fetch "$L/bin/lord-fetch"
[ -x "$L/bin/lord-fetch" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-fetch" 2>/dev/null || true; }
# the app catalog (0.8.5, apt/): Store → Artificial Intelligence; pkg-source-add reads it as root
install -d "$L/apt/keys"
install -m 644 apt/catalog.json "$L/apt/catalog.json"
for k in apt/keys/*.asc; do install -m 644 "$k" "$L/apt/keys/"; done
# lord-tls (0.8.5): HTTPS for the package engine's downloads, sandboxed (docs/PACKAGES.md, "HTTPS")
[ -x server/build/lord-tls ] && install -m 755 server/build/lord-tls "$L/bin/lord-tls"
[ -x "$L/bin/lord-tls" ] && { "$STRIP" --strip-unneeded "$L/bin/lord-tls" 2>/dev/null || true; }
# FIDO2 security keys reachable by the session user (lord-sessiond sessions have no logind seat)
install -d "$PKG/usr/lib/udev/rules.d"
install -m 644 packaging/udev/71-lordos-fido.rules "$PKG/usr/lib/udev/rules.d/71-lordos-fido.rules"
for d in themes/*/; do
    n=$(basename "$d")
    [ -f "$d/theme.bundle.js" ] || continue
    install -d "$L/themes/$n"
    install -m 644 "$d/theme.json" "$d/theme.bundle.js" "$L/themes/$n/"
    [ -d "$d/assets" ] && cp -r "$d/assets" "$L/themes/$n/"
done
cp -r assets/fonts assets/locales assets/shell assets/icons assets/mascot assets/monitor assets/help assets/fw assets/cleaner assets/machines assets/webapps "$L/assets/"
# the makers' keys Machines' download catalog pins (assets/machines/systems.json "keys"; docs/VIRTUALIZATION.md 3.9):
# the admin helper checks each checksum list's signature with them before it trusts a hash
install -d "$L/assets/machines/keys"
install -m 644 packaging/keys/machines/*.asc "$L/assets/machines/keys/"
# the wallpaper collection (assets/wallpapers/README.md): once, in the standard place for desktop
# backgrounds; lord-server reaches it as asset:wallpapers/… through the link. Only the JPEGs, the
# thumbnails and the README (index.json is bundled into the SDK at build time); motion.json and the built-in
# moving wallpaper (docs/WALLPAPERS.md).
install -d "$PKG/usr/share/backgrounds/lordos"
( cd assets/wallpapers && find . -name '*.jpg' -o -name README.md -o -name motion.json -o -name '*.mp4' ) | while read -r f; do
    install -D -m 644 "assets/wallpapers/$f" "$PKG/usr/share/backgrounds/lordos/$f"
done
ln -s /usr/share/backgrounds/lordos "$L/assets/wallpapers"
# each isolated app's process (lord-server --app) loads its runtime from here
[ -f assets/app-host.bundle.js ] || { echo "missing assets/app-host.bundle.js (npm run build)" >&2; exit 1; }
install -m 644 assets/app-host.bundle.js "$L/assets/app-host.bundle.js"
# system apps that run as their own sandboxed process (manifest "process": "isolated"; e.g. Passwords)
[ -d assets/sysapps ] && cp -r assets/sysapps "$L/assets/"
# verified system code (docs/SIGNING.md): the manifest of every bundle above and, for a release, its
# signature made on the maintainer's machine (tools/release/sign-code.sh). lord-server checks the
# staged tree exactly as a session will; a release build (RELEASE=1) needs the signature.
[ -f code-manifest.json ] || { echo "missing code-manifest.json (npm run build writes it)" >&2; exit 1; }
install -m 644 code-manifest.json "$L/code-manifest.json"
[ -f code-manifest.sig ] && install -m 644 code-manifest.sig "$L/code-manifest.sig"
if [ "${REQUIRE_SIGNED_CODE:-0}" = 1 ] && [ ! -f "$L/code-manifest.sig" ]; then
    echo "missing code-manifest.sig: sign the manifest on the maintainer's machine (tools/release/sign-code.sh)" >&2; exit 1
fi
# a release: every app that changed since the previous release has a new version (tools/app-versions.mjs, #91)
if [ "${REQUIRE_SIGNED_CODE:-0}" = 1 ] && [ -d .git ]; then node tools/app-versions.mjs || exit 1; fi
# (0.8.7) the tested-hardware snapshot for System Information's Compatibility panel is not in this package: it
# is lordos-hwdata (tools/iso/mkdeb-hwdata.sh, Depends below), so a newer one can be published between releases
# the check is the same on any architecture: building another architecture's package on this host
# (tools/rpi/build-img.sh on x86-64 with an aarch64 server/build), CODE_CHECK_SERVER names a
# lord-server built for this host from the same tree
"${CODE_CHECK_SERVER:-server/build/lord-server}" --check-code "$L" > "$PKG/code-check.log" 2>&1 || {
    tail -20 "$PKG/code-check.log" >&2; echo "the staged system code does not match code-manifest.json" >&2; exit 1; }
tail -1 "$PKG/code-check.log"; rm -f "$PKG/code-check.log"
install -m 755 tools/iso/rootfs/usr/lib/lordos/lordos-live-prep "$L/lordos-live-prep"
# updates in place (docs/UPDATES.md): the repository, its key, the migrations and the restart notice
install -D -m 644 packaging/apt/lordos.sources "$L/apt/lordos.sources"
install -D -m 644 packaging/keys/lordos-archive-keyring.gpg "$PKG/usr/share/keyrings/lordos-archive-keyring.gpg"
install -m 755 packaging/migrations/migrate "$L/migrate"
install -d "$L/migrations"
install -m 644 packaging/migrations/[0-9][0-9][0-9]-*.sh "$L/migrations/"
install -D -m 755 packaging/kernel/zz-lordos-reboot-required "$PKG/etc/kernel/postinst.d/zz-lordos-reboot-required"
install -D -m 644 tools/iso/rootfs/etc/wireplumber/wireplumber.conf.d/lordos-bluez.conf "$PKG/etc/wireplumber/wireplumber.conf.d/lordos-bluez.conf"
# a Bluetooth headset plays in high fidelity until its microphone is chosen (issue #78, docs/SOUND.md)
install -D -m 644 tools/iso/rootfs/etc/wireplumber/wireplumber.conf.d/lordos-bluez-profiles.conf "$PKG/etc/wireplumber/wireplumber.conf.d/lordos-bluez-profiles.conf"
install -D -m 644 tools/iso/rootfs/etc/wireplumber/wireplumber.conf.d/lordos-volume.conf "$PKG/etc/wireplumber/wireplumber.conf.d/lordos-volume.conf"
install -D -m 644 tools/iso/rootfs/etc/pipewire/pipewire-pulse.conf.d/lordos-no-modules.conf "$PKG/etc/pipewire/pipewire-pulse.conf.d/lordos-no-modules.conf"
# in a virtual machine only: more headroom for emulated sound cards (docs/SOUND.md, "Virtual machines")
install -D -m 644 tools/iso/rootfs/etc/wireplumber/wireplumber.conf.d/lordos-vm-audio.conf "$PKG/etc/wireplumber/wireplumber.conf.d/lordos-vm-audio.conf"
# vmwgfx only on VMware itself; elsewhere it removes the firmware framebuffer and fails (docs/ISO.md)
install -D -m 644 tools/iso/rootfs/etc/modprobe.d/lordos-vmwgfx.conf "$PKG/etc/modprobe.d/lordos-vmwgfx.conf"
# SATA disk temperatures for Monitor → Disks: the drivetemp module at boot (docs/MONITOR.md)
install -D -m 644 tools/iso/rootfs/etc/modules-load.d/lordos-drivetemp.conf "$PKG/usr/lib/modules-load.d/lordos-drivetemp.conf"
# Debian's smartd stays off: Disks and the Monitor read SMART on demand (docs/SYSTEM.md, "smartd")
install -D -m 644 packaging/systemd/smartmontools-lordos.conf "$PKG/usr/lib/systemd/system/smartmontools.service.d/lordos.conf"
# gnome-keyring, if an app pulls it in, without its Secret Service: LordOS keeps that name (docs/PASSWORDS.md)
install -D -m 644 packaging/systemd/gnome-keyring-lordos.conf "$PKG/usr/lib/systemd/user/gnome-keyring-daemon.service.d/lordos.conf"
# Machines: a shutdown or restart waits up to 30 s for running machines to be saved (docs/VIRTUALIZATION.md 6.1)
install -D -m 644 packaging/systemd/logind-lordos-machines.conf "$PKG/usr/lib/systemd/logind.conf.d/lordos-machines.conf"
# remote login (SSH) defaults, read only once openssh-server is installed (docs/REMOTE.md)
install -D -m 644 tools/iso/rootfs/etc/ssh/sshd_config.d/10-lordos.conf "$PKG/etc/ssh/sshd_config.d/10-lordos.conf"
# a process may only trace its own descendants: a restricted Linux app must not take over an
# unrestricted process of the same user (lord-server, PipeWire, another app) and act through it
install -d "$PKG/usr/lib/sysctl.d"
printf '%s\n' '# LordOS: ptrace only of descendants (docs/FIREWALL.md, "Linux apps")' 'kernel.yama.ptrace_scope = 1' > "$PKG/usr/lib/sysctl.d/50-lordos-ptrace.conf"
chmod 644 "$PKG/usr/lib/sysctl.d/50-lordos-ptrace.conf"
install -m 644 tools/iso/rootfs/etc/systemd/system/lordos.service "$PKG/usr/lib/systemd/system/lordos.service"
# smooth video in Chromium without hardware decoding (docs/VIDEO.md); the Firefox manifest stays out
X="$PKG/usr/share/chromium/extensions/lordos-video"
install -d "$X"
(cd assets/browser/video && find . -type f ! -name manifest.firefox.json ! -name '.*' -print) | while read -r f; do
    install -D -m 644 "assets/browser/video/$f" "$X/$f"
done
find "$PKG/usr/share/chromium" -type d -exec chmod 755 {} +
# boot splash (docs/brand/README.md, "Boot splash"): the Plymouth theme and the hand-off to LordOS
install -m 755 tools/boot/lordos-splash "$L/lordos-splash"
install -m 644 tools/boot/systemd/lordos-splash-quit.service "$PKG/usr/lib/systemd/system/lordos-splash-quit.service"
install -D -m 644 tools/boot/systemd/plymouth-quit.service.d/lordos.conf "$PKG/usr/lib/systemd/system/plymouth-quit.service.d/lordos.conf"
install -d "$PKG/usr/share/plymouth/themes/lordos"
install -m 644 tools/boot/plymouth/lordos/lordos.plymouth tools/boot/plymouth/lordos/lordos.script \
    tools/boot/plymouth/lordos/*.png "$PKG/usr/share/plymouth/themes/lordos/"
# boot menu theme: both architectures, the arm64 ISO and Debian on arm64 boot with GRUB (the same arm64
# package goes on the Raspberry Pi, which boots without GRUB and doesn't use it)
install -d "$PKG/usr/share/grub/themes/lordos"
install -m 644 tools/iso/grub/lordos/theme.txt tools/iso/grub/lordos/*.png tools/iso/grub/lordos/*.pf2 \
    "$PKG/usr/share/grub/themes/lordos/"
# LordOS GTK3 themes (the settings portal picks them by light/dark)
install -d "$PKG/usr/share/themes"
cp -r assets/gtk/LordOS assets/gtk/LordOS-dark "$PKG/usr/share/themes/"
find "$PKG/usr/share/themes" -type d -exec chmod 755 {} +; find "$PKG/usr/share/themes" -type f -exec chmod 644 {} +
# GTK's defaults where the session can't give its own: the window buttons of apps that draw their
# title bar (#52; docs/WAYLAND.md). GLib's dpkg trigger compiles the schema override.
install -D -m 644 assets/gtk/90_lordos.gschema.override "$PKG/usr/share/glib-2.0/schemas/90_lordos.gschema.override"
install -D -m 644 assets/gtk/settings.ini "$PKG/etc/gtk-3.0/settings.ini"
install -D -m 644 assets/gtk/settings.ini "$PKG/etc/gtk-4.0/settings.ini"
install -m 644 tools/iso/rootfs/etc/systemd/system/lord-netperm.service "$PKG/usr/lib/systemd/system/lord-netperm.service"
# fans by temperature from the first boot (docs/FANS.md): enabled by postinst, also in a chroot
install -m 644 tools/iso/rootfs/etc/systemd/system/lord-fand.service "$PKG/usr/lib/systemd/system/lord-fand.service"
install -m 644 tools/iso/rootfs/etc/systemd/system/lordos-autoinstall.service "$PKG/usr/lib/systemd/system/lordos-autoinstall.service"
find "$L" -type d -exec chmod 755 {} +
find "$L/themes" "$L/assets" "$L/migrations" "$L/apt" -type f -exec chmod 644 {} +

D="$PKG/usr/share/doc/lordos"
install -m 644 README.md "$D/README.md"
cat > "$D/copyright" <<'C'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: LordOS
Source: https://github.com/lordbasex/LordOS

Files: *
Copyright: 2026 iPERFEX and the LordOS contributors
License: Expat
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

Files: usr/share/backgrounds/lordos/*
Copyright: 2026 iPERFEX
License: Expat
Comment: Created by iPERFEX with an AI image generator; part of the LordOS brand
 assets. The LordOS name, the crown and Lumo shown in them are also covered by the
 LordOS trademark and brand policy (lordos.org).

Files: usr/lib/lordos/assets/fonts/*
License: OFL-1.1
 Inter, JetBrains Mono and Plus Jakarta Sans: SIL Open Font License 1.1 (each
 font's license text ships next to it, *-LICENSE.txt).
 On Debian: /usr/share/common-licenses does not include it; see https://openfontlicense.org

Files: usr/lib/lordos/bin/lord-bench
License: OFL-1.1
 Built into lord-bench for LordBench's shaping workload: subsets of Noto Sans Arabic,
 Devanagari, Thai and CJK SC. SIL Open Font License 1.1, full text and copyright
 notices in usr/share/doc/lordos/OFL-Noto.txt.

Files: usr/lib/lordos/assets/monitor/geoip-country.mmdb.xz
Copyright: db-ip.com
License: CC-BY-4.0
 DB-IP Lite Country (https://db-ip.com), converted by tools/gen-monitor-data.py.
 https://creativecommons.org/licenses/by/4.0/

Files: usr/lib/lordos/assets/monitor/land.bin usr/lib/lordos/assets/locales/*/countries.json
License: public-domain
 Natural Earth (https://www.naturalearthdata.com).

Files: third_party/quickjs/* third_party/stb/*
License: MIT and public-domain
 QuickJS (MIT) and stb (public domain / MIT), compiled into lord-server.
C
chmod 644 "$D/copyright"
install -m 644 server/src/bench/fonts/OFL.txt "$D/OFL-Noto.txt"
printf 'lordos (%s) trixie; urgency=medium\n\n  * Package built by tools/iso/mkdeb.sh.\n\n -- LordOS <lord.basex@gmail.com>  %s\n' \
    "$VERSION" "$(date -R)" | gzip -9n > "$D/changelog.gz"
chmod 644 "$D/changelog.gz"

# with the GPU path (make GPU=1), the Mesa libraries are dependencies too
GPUDEPS=""
# readelf (not ldd): also works when building for another architecture (tools/rpi)
if readelf -d "$L/bin/lord-server" 2>/dev/null | grep -q "NEEDED.*libgbm"; then GPUDEPS=", libgbm1, libegl1, libgles2"; fi
# the test bench's 3D view draws through wayland-egl (built only when Mesa's EGL was there)
if readelf -d "$PKG/usr/bin/lordos-testbench" 2>/dev/null | grep -q "NEEDED.*libwayland-egl"; then
    case "$GPUDEPS" in *libegl1*) ;; *) GPUDEPS="$GPUDEPS, libegl1, libgles2" ;; esac
    GPUDEPS="$GPUDEPS, libwayland-egl1"
fi
DOCDEPS=""
if [ -x "$L/bin/lord-doc" ]; then DOCDEPS=", libpoppler-glib8t64, libarchive13t64, libcairo2, libfreetype6, libfontconfig1, poppler-data"; fi
# lord-ocr: the library and English (libtesseract5 brings no language data); the interface's other languages
# (the "fast" data, ~2 MB each) are Recommends
OCRDEPS=""; OCRRECS=""
if [ -x "$L/bin/lord-ocr" ]; then OCRDEPS=", libtesseract5, tesseract-ocr-eng"; OCRRECS=", tesseract-ocr-spa, tesseract-ocr-por, tesseract-ocr-fra, tesseract-ocr-ita"; fi
# lord-tts: Vulkan's loader (the GPU is optional, the CPU always works), OpenMP and the C++ runtime
TTSDEPS=""
if [ -x "$L/bin/lord-tts" ]; then TTSDEPS=", libvulkan1, libgomp1, libstdc++6"; fi
MEDIADEPS=""
if [ -x "$L/bin/lord-media" ]; then MEDIADEPS=", libavcodec61, libavformat61, libavutil59, libswscale8, libswresample5, libva2, libva-drm2"; fi
# the remote desktop is WebRTC only (docs/REMOTE.md, "Video (WebRTC)"): lord-sessiond always links libsrtp2 and usrsctp
RTCDEPS=", libsrtp2-1, libusrsctp2"
# the CPU's microcode updates on amd64 (non-free-firmware, in every LordOS install's sources): a Depends, so the
# update to 0.8.8.6 brings them to installs that never had them (they load on the next boot)
MICROCODEDEPS=""
[ "${DEB_ARCH:-$(dpkg --print-architecture)}" = amd64 ] && MICROCODEDEPS=", intel-microcode, amd64-microcode"
if ! objdump -p "$L/bin/lord-sessiond" 2>/dev/null | grep -q "libsrtp2"; then echo "lord-sessiond is not linked with libsrtp2: rebuild it (make -C server)" >&2; exit 1; fi
# the signed tested-hardware snapshot, its own package (tools/iso/mkdeb-hwdata.sh): any version of format 1,
# the first being the snapshot of 2026-10-27 (a format 2 is a Breaks in lordos-hwdata, not a change here)
HWDATADEPS=", lordos-hwdata (>= 2026.10.27)"
# rclone for Cloud and Connect to Server, rclone's signed release binary at /usr/lib/lordos/rclone (tools/iso/mkdeb-rclone.sh, docs/CLOUD.md 3.1);
# Debian's rclone (1.60) is too old for Cloud, and nothing runs /usr/bin/rclone any more
RCLONEDEPS=", lordos-rclone (>= 1.75.2)"
SIZE=$(du -sk "$PKG/usr" | awk '{s += $1} END {print s}')
cat > "$PKG/DEBIAN/control" <<C
Package: lordos
Version: $VERSION
Architecture: ${DEB_ARCH:-$(dpkg --print-architecture)}
Maintainer: LordOS <lord.basex@gmail.com>
Installed-Size: $SIZE
Depends: libc6 (>= 2.38), libwayland-server0 (>= 1.22), libwayland-client0 (>= 1.22), libxkbcommon0 (>= 1.5), libcrypt1, libssl3t64 | libssl3, ca-certificates, passwd, procps, xkb-data, libfreerdp3-3, libfreerdp-client3-3, libwinpr3-3, fuse3, openssh-client, openssl, polkitd, pkexec$HWDATADEPS$RCLONEDEPS$GPUDEPS$DOCDEPS$OCRDEPS$TTSDEPS$MEDIADEPS$RTCDEPS$MICROCODEDEPS
Recommends: pipewire-pulse, wireplumber, wpasupplicant, openvpn, fonts-noto-core, fonts-noto-color-emoji, fonts-dejavu-core, locales, tzdata, sudo, cups, cups-client, avahi-daemon, libnss-mdns, mesa-va-drivers, bluez, libspa-0.2-bluetooth, power-profiles-daemon, systemd-timesyncd, shared-mime-info, gsettings-desktop-schemas, xwayland, pci.ids, usb.ids, curl, wget, rsync, screen, htop, vim, mc, tcpdump, net-tools, fdisk, rfkill, jq, whiptail, zstd$OCRRECS
Section: x11
Priority: optional
Protected: yes
Homepage: https://github.com/lordbasex/LordOS
Description: desktop environment for Debian GNU/Linux, built from scratch
 LordOS is a desktop environment for Debian GNU/Linux with its own display
 server and Wayland compositor written in C (DRM/KMS, evdev) and the
 interface in React on its own engine.
 It includes the native apps, the themes, the login screen and the
 lordos.service service (lord-sessiond) that starts it on tty1. Remote desktop
 (browser, port 5673) ships turned off.
C
# lordos.service is enabled by hand (the ISO does it; on an installed system the user decides).
# lord-fand.service is always enabled (and started on a running system): fans need no choice.
cat > "$PKG/DEBIAN/postinst" <<'C'
#!/bin/sh
set -e
if [ "$1" = configure ] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable lord-fand.service >/dev/null 2>&1 || true
    if [ -d /run/systemd/system ]; then systemctl restart systemd-sysctl.service >/dev/null 2>&1 || true; fi
    if [ -d /run/systemd/system ]; then systemctl restart lord-fand.service >/dev/null 2>&1 || true; fi
fi
# the LordOS repository: added when missing, so an administrator's edit (Enabled: no) is kept; not a
# conffile, so removing the package also removes it and apt keeps working (docs/UPDATES.md)
if [ "$1" = configure ] && [ ! -e /etc/apt/sources.list.d/lordos.sources ] && [ -d /etc/apt/sources.list.d ]; then
    install -m 644 /usr/lib/lordos/apt/lordos.sources /etc/apt/sources.list.d/lordos.sources
fi
# pkexec reaches the session's authentication agent through lord-pkexec (docs/PRIVACY.md, "Authentication
# for Linux apps"); the real one keeps its setuid bit under another name, and its updates land there
if [ "$1" = configure ]; then
    dpkg-divert --package lordos --quiet --rename --divert /usr/bin/pkexec.distrib --add /usr/bin/pkexec
    ln -sfn ../lib/lordos/bin/lord-pkexec /usr/bin/pkexec
fi
# `say` is LordOS's text to speech (docs/VOICE.md); an alternative, so another package's say can sit beside it
if [ "$1" = configure ] && [ -x /usr/lib/lordos/bin/lord-say ]; then
    update-alternatives --quiet --install /usr/bin/say say /usr/lib/lordos/bin/lord-say 50
fi
# version-to-version system changes, each applied once (docs/UPDATES.md, "Migrations")
if [ "$1" = configure ] && [ -x /usr/lib/lordos/migrate ]; then
    /usr/lib/lordos/migrate "${2:-}"
fi
# an upgrade replaced the desktop under the running sessions: Store → Updates offers a restart
# (the Debian convention /run/reboot-required; never on a new install or in an image build)
if [ "$1" = configure ] && [ -n "${2:-}" ] && [ -d /run/systemd/system ] &&
   [ "$2" != "$(dpkg-query -W -f '${Version}' lordos 2>/dev/null || true)" ]; then
    touch /run/reboot-required 2>/dev/null && echo lordos >> /run/reboot-required.pkgs 2>/dev/null || true
fi
# the boot splash lives in the initramfs: when it is the selected theme, rebuild it with the new images
# (initramfs-tools' trigger; the theme is selected by the ISO, the Pi image or install.sh, never here)
if [ "$1" = configure ] && [ -x /usr/sbin/plymouth-set-default-theme ] &&
   [ "$(/usr/sbin/plymouth-set-default-theme 2>/dev/null)" = lordos ] && command -v update-initramfs >/dev/null 2>&1; then
    dpkg-trigger --no-await update-initramfs 2>/dev/null || true
fi
C
chmod 755 "$PKG/DEBIAN/postinst"
# removing the package gives the fans back first (ExecStopPost restores the kernel's control)
cat > "$PKG/DEBIAN/prerm" <<'C'
#!/bin/sh
set -e
if [ "$1" = remove ] && command -v systemctl >/dev/null 2>&1; then
    if [ -d /run/systemd/system ]; then systemctl stop lord-fand.service >/dev/null 2>&1 || true; fi
    systemctl disable lord-fand.service >/dev/null 2>&1 || true
fi
if [ "$1" = remove ]; then update-alternatives --quiet --remove say /usr/lib/lordos/bin/lord-say 2>/dev/null || true; fi
# the boot splash theme goes away with the package: Plymouth back to its default theme first
if [ "$1" = remove ] && [ -x /usr/sbin/plymouth-set-default-theme ] &&
   [ "$(/usr/sbin/plymouth-set-default-theme 2>/dev/null)" = lordos ]; then
    /usr/sbin/plymouth-set-default-theme --reset >/dev/null 2>&1 || true
    command -v update-initramfs >/dev/null 2>&1 && { dpkg-trigger --no-await update-initramfs 2>/dev/null || true; }
fi
C
chmod 755 "$PKG/DEBIAN/prerm"
# without the package its key is gone: take the repository out of apt's sources too
cat > "$PKG/DEBIAN/postrm" <<'C'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then rm -f /etc/apt/sources.list.d/lordos.sources; fi
# the browsers' video settings written by Settings (video-optimize, docs/VIDEO.md)
if [ "$1" = remove ] || [ "$1" = purge ]; then rm -f /etc/chromium/policies/managed/lordos-video.json /etc/firefox-esr/lordos-video.js /etc/chromium/policies/recommended/lordos-session.json /etc/firefox-esr/lordos-session.js; fi
if [ "$1" = purge ]; then rm -rf /var/lib/lordos/migrations; fi
# the real pkexec back in its place
if [ "$1" = remove ] || [ "$1" = purge ]; then
    if [ -L /usr/bin/pkexec ]; then rm -f /usr/bin/pkexec; fi
    dpkg-divert --package lordos --quiet --rename --remove /usr/bin/pkexec || true
fi
C
chmod 755 "$PKG/DEBIAN/postrm"
# files under /etc are conffiles (lintian: file-in-etc-not-marked-as-conffile)
( cd "$PKG" && find etc -type f | sed 's|^|/|' ) > "$PKG/DEBIAN/conffiles"
chmod 644 "$PKG/DEBIAN/conffiles"
dpkg-deb --root-owner-group --build "$PKG" "$OUT" >/dev/null
echo "$OUT ($(du -h "$OUT" | cut -f1))"
