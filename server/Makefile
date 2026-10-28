# lord-server — libc + kernel headers + libxkbcommon + libwayland-server + vendored QuickJS and stb.
# No libdrm, libinput, X11 or WebKit; the Wayland compositor (src/wl) and the terminal (src/term) are our own.
CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra
CPPFLAGS += -D_GNU_SOURCE -Isrc -Ibuild/gen -isystem ../third_party/quickjs -isystem ../third_party/stb
LDLIBS  += -lm -lpthread -lxkbcommon -lwayland-server -lwayland-client -lcrypt

# Architecture: x86_64 (PC) or aarch64 (Raspberry Pi 5/4 and other ARM64, see docs/RASPBERRY.md). Never
# -march=native: the .deb has to run on any CPU of the architecture. On aarch64 NEON is
# part of ARMv8-A (no -mfpu needed) and canvas.c's NEON loops are used (NEON=0 turns them off;
# tests/canvas_test.c compares them with the C ones).
ARCH ?= $(shell $(CC) -dumpmachine 2>/dev/null | cut -d- -f1)
ifeq ($(ARCH),aarch64)
NEON ?= 1
ifeq ($(NEON),1)
CPPFLAGS += -DLORD_NEON
endif
endif

# Wayland protocols: wayland-scanner generates the code from the wayland-protocols XML files.
WLPROTO := /usr/share/wayland-protocols
WL_XML  := $(WLPROTO)/stable/xdg-shell/xdg-shell.xml $(WLPROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
WL_GEN  := build/gen/xdg-shell-protocol.h build/gen/xdg-decoration-unstable-v1-protocol.h
WL_OBJ  := build/gen/xdg-shell-protocol.o build/gen/xdg-decoration-unstable-v1-protocol.o
# dmabuf, presentation, viewporter and mouse for games (M7/GPU): they also work on the CPU path
WL_MORE := stable/linux-dmabuf/linux-dmabuf-v1 stable/presentation-time/presentation-time stable/viewporter/viewporter \
           unstable/relative-pointer/relative-pointer-unstable-v1 unstable/pointer-constraints/pointer-constraints-unstable-v1 \
           unstable/pointer-gestures/pointer-gestures-unstable-v1 \
           staging/cursor-shape/cursor-shape-v1 unstable/tablet/tablet-unstable-v2 \
           staging/fractional-scale/fractional-scale-v1 unstable/xdg-output/xdg-output-unstable-v1 \
           unstable/primary-selection/primary-selection-unstable-v1 unstable/idle-inhibit/idle-inhibit-unstable-v1
WL_GEN  += $(foreach p,$(WL_MORE),build/gen/$(notdir $(p))-protocol.h)
WL_OBJ  += $(foreach p,$(WL_MORE),build/gen/$(notdir $(p))-protocol.o)

# GPU compositing (src/gpu): Mesa (GBM + EGL + GLES2) if its -dev packages are present; otherwise (or with
# GPU=0) it's built without GPU and lord-server always composites on the CPU. See docs/GPU.md.
GPU ?= $(shell pkg-config --exists gbm egl glesv2 2>/dev/null && echo 1 || echo 0)
ifeq ($(GPU),1)
CPPFLAGS += -DLORD_GPU $(shell pkg-config --cflags gbm egl glesv2)
LDLIBS   += $(shell pkg-config --libs gbm egl glesv2)
endif

# Wayland client headers for app mode (see CLIENT_PROTO; the server defines the interfaces)
APP_CLIENT_HDR := build/gen/client/xdg-shell-client-protocol.h build/gen/client/xdg-decoration-unstable-v1-client-protocol.h \
                  build/gen/client/pointer-gestures-unstable-v1-client-protocol.h
QJS_DIR := ../third_party/quickjs
QJS_OBJ := $(patsubst %,build/qjs/%.o,quickjs libregexp libunicode cutils dtoa)

# .lpk app isolation, permissions and signing (src/sec, see docs/PRIVACY.md and docs/SIGNING.md)
# Monitor: metrics, connections and the country of an IP (src/monitor, see docs/MONITOR.md)
MON_SRC := src/monitor/mon.c src/monitor/disktemp.c src/monitor/nethist.c src/monitor/netrec.c src/monitor/conn.c src/monitor/procctl.c src/monitor/proc_admin.c src/monitor/mmdb.c src/monitor/map.c src/ui/js_monitor.c
# Media (src/media, docs/MEDIA.md): only the lord-server side. Only lord-media links FFmpeg.
MEDIA_SRC := src/media/media.c src/media/hwdec.c src/media/mview.c src/media/capture.c src/media/annot.c src/media/maudio.c src/media/mpulse.c src/media/proto.c src/media/mstream.c src/ui/js_media.c
SEC_SRC := src/sec/json.c src/sec/sign.c src/sec/pkgstate.c src/sec/perms.c src/sec/cgroup.c src/sec/sandbox.c src/sec/brokersrv.c src/sec/brokercam.c src/sec/brokerkr.c src/sec/brokermail.c src/sec/restrict.c src/sec/appid.c src/sec/pwguard.c src/sec/codesig.c
# App mode: lord-server --app <id> runs an .lpk app as an isolated Wayland client (src/app)
APP_SRC := src/app/appclient.c src/app/appspawn.c src/app/probe.c src/ui/js_broker.c src/ui/js_brokercam.c src/ui/js_brokermail.c src/app/apphost_bc.c
SRC := src/main.c src/kms.c src/gpu/gpu.c src/input.c src/canvas.c src/par.c src/admin.c src/install/install.c src/apps.c src/lpk.c src/fsops.c src/remote/mirror.c src/remote/remote_admin.c src/remote/ipnet.c src/remote/rtcwire.c src/remote/ssh_admin.c src/remote/ssh_root.c src/users/users_admin.c src/users/homedirs.c src/keyboard.c src/keylag.c src/ime.c src/wl/wl.c src/wl/x11wire.c src/wl/xwm.c src/wl/xwayland.c \
       src/ui/node.c src/ui/style.c src/ui/layout.c src/ui/transform.c src/ui/text.c src/ui/image.c \
       src/ui/paint.c src/ui/js.c src/ui/js_sys.c src/ui/js_term.c src/ui/js_wasm.c src/wasm/wasm.c src/wasm/wasm_run.c src/ui/js_sound.c src/ui/js_net.c src/sound/snd.c src/sound/pulse.c src/ui/otf.c src/ui/fonts.c \
       src/ui/unicode.c src/ui/bidi.c src/ui/shape.c src/ui/shape_indic.c src/ui/shape_syl.c src/ui/shape_khmer.c src/ui/shape_myanmar.c src/ui/shape_use.c src/ui/thbrk.c src/ui/textlayout.c \
       src/term/vt.c src/term/pty.c src/term/term.c src/term/term_paint.c src/term/hold.c \
       src/net/nl.c src/net/crypto.c src/net/state.c src/net/sys.c src/net/dhcp.c src/net/wifi.c src/net/wg.c src/net/ovpn.c src/net/vpn.c src/net/vici.c src/net/ipsec.c src/net/vpnproc.c src/net/actions.c \
       src/pkg/crypto.c src/pkg/pgp.c src/pkg/xz.c src/pkg/deb.c src/pkg/idx.c src/pkg/pin.c src/pkg/status.c src/pkg/resolve.c src/pkg/http.c src/pkg/pkg.c src/pkg/sources_admin.c src/ui/js_pkg.c src/ui/js_sec.c \
       $(SEC_SRC) $(APP_SRC) $(MON_SRC) $(MEDIA_SRC)
# Documents and printing (M9): lord-doc helper sessions and our own IPP/mDNS/eSCL client
# (src/doc, src/print; see docs/DOCUMENTS.md and docs/PRINTING.md). No Poppler or libarchive here.
DOC_SRC := src/doc/doc.c src/doc/wire.c src/ui/js_doc.c src/print/ipp.c src/print/phttp.c src/print/mdns.c src/print/cups.c \
           src/print/escl.c src/print/print.c src/ui/js_print.c
SRC += $(DOC_SRC)
# Per-app firewall (src/fw, docs/FIREWALL.md): rules, decision and names learned from DNS
FW_SRC := src/fw/rules.c src/fw/dnsmap.c src/fw/fwmap.c src/fw/fwlists.c src/fw/fwsub.c src/ui/js_fw.c src/sec/netconn.c
SRC += $(FW_SRC)
OBJ := $(SRC:src/%.c=build/obj/%.o)
HDR := $(wildcard src/*.h src/gpu/*.h src/ui/*.h src/term/*.h src/wl/*.h src/wasm/*.h src/sound/*.h src/pkg/*.h src/net/*.h src/install/*.h src/remote/*.h src/users/*.h src/sec/*.h src/monitor/*.h src/media/*.h src/app/*.h src/doc/*.h src/print/*.h) $(WL_GEN) $(APP_CLIENT_HDR)
HDR += $(wildcard src/fw/*.h)
# Touchpad and touchscreens: our own driver (src/tp, no libinput), gestures and touches for the
# Wayland apps (src/wl/wl_input.c) and lord.input (src/ui/js_input.c). See docs/TRACKPAD.md.
TP_SRC := src/tp/tp.c src/tp/tp_route.c src/wl/wl_input.c src/ui/js_input.c
SRC += $(TP_SRC)
# Window management: dragging in C, a layer for the window being moved and our own cursors (docs/WINDOWS.md).
# cursor-shape-v1 needs the tablet-v2 interfaces (they're only linked, not advertised).
# GTK/GNOME/Qt apps: our own org.freedesktop.portal.Settings (src/ui/portal.c) with the D-Bus client from
# Passwords (src/vault/dbus.c, docs/PASSWORDS.md).
WM_SRC := src/cursor.c src/ui/grab.c src/ui/js_wm.c src/ui/portal.c src/vault/dbus.c
SRC += $(WM_SRC)
# Clock and Help (docs/SYSTEM.md): time zones with our own TZif reader, and the help articles
CLOCK_SRC := src/ui/tzif.c src/ui/js_help.c
SRC += $(CLOCK_SRC)
# Logs (docs/SYSTEM.md): our own systemd journal reader, the logs-system action and lord.logs
LOGS_SRC := src/logs/journal.c src/logs/logs_admin.c src/ui/js_logs.c
SRC += $(LOGS_SRC)
HDR += $(wildcard src/logs/*.h)
# Backups (docs/SYSTEM.md): hard-linked snapshots in a low-priority thread
BACKUP_SRC := src/backup/engine.c src/backup/crypt.c src/backup/backup.c src/ui/js_backup.c
SRC += $(BACKUP_SRC)
HDR += $(wildcard src/backup/*.h)
# optional encryption uses Monocypher (vendored in third_party/monocypher; the object comes from mk/vault.mk)
build/obj/backup/%.o: CPPFLAGS += -isystem ../third_party/monocypher
# Disks (docs/SYSTEM.md): disks, partitions and mounts without root, our own SMART and the disk-* actions
DISK_SRC := src/disk/disk.c src/disk/smart.c src/disk/smartctl.c src/disk/disk_admin.c src/disk/smartctl_admin.c src/ui/js_disk.c
SRC += $(DISK_SRC)
HDR += $(wildcard src/disk/*.h)
# LordOS Imager (docs/IMAGER.md): the imager-* actions (verified image → removable disk, downloads
# as the user) and lordos-firstrun, the Raspberry Pi first-run file applier (src/firstrun)
IMAGER_SRC := src/imager/imgverify.c src/imager/imager_admin.c src/ui/js_imager.c
SRC += $(IMAGER_SRC)
HDR += $(wildcard src/imager/*.h)
FIRSTRUN_SRC := src/firstrun/firstrun.c src/firstrun/firstrun_apply.c src/users/homedirs.c
ALL += build/lordos-firstrun
build/lordos-firstrun: $(FIRSTRUN_SRC) src/firstrun/firstrun.h src/users/homedirs.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(FIRSTRUN_SRC)
test: build/firstrun-test build/firstrun-fuzz build/imager-test
build/firstrun-test: tests/firstrun_test.c src/firstrun/firstrun.c src/firstrun/firstrun.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/firstrun_test.c src/firstrun/firstrun.c
	./$@ || { rm -f $@; exit 1; }
build/firstrun-fuzz: tests/firstrun_test.c src/firstrun/firstrun.c src/firstrun/firstrun.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ tests/firstrun_test.c src/firstrun/firstrun.c
	./$@ 300000 || { rm -f $@; exit 1; }
build/imager-test: tests/imager_test.c src/imager/imgverify.c src/imager/imgverify.h src/pkg/crypto.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ tests/imager_test.c src/imager/imgverify.c src/pkg/crypto.c
	./$@ || { rm -f $@; exit 1; }
OBJ := $(SRC:src/%.c=build/obj/%.o)
HDR += $(wildcard src/tp/*.h)

.PHONY: all clean test test-full drag-golden
# lord-sessiond: sessions, login screen and remote desktop (docs/SESSIONS.md). It's the
# only one that links OpenSSL (TLS), and with it libsrtp2 and usrsctp (the remote desktop's WebRTC);
# without libssl-dev, only lord-server is built.
SESSIOND ?= $(shell pkg-config --exists openssl 2>/dev/null && echo 1 || echo 0)
ALL += build/lord-server
ifeq ($(SESSIOND),1)
ALL += build/lord-sessiond
endif
# lord-media: the media helper (docs/MEDIA.md). It's the only one that links FFmpeg (and libva);
# lord-server never does. If FFmpeg's -dev packages are missing, everything else is built without it.
MEDIA ?= $(shell pkg-config --exists libavcodec libavformat libavutil libswscale libswresample 2>/dev/null && echo 1 || echo 0)
ifeq ($(MEDIA),1)
ALL += build/lord-media
endif
# lord-doc: the document helper (docs/DOCUMENTS.md). It's the only one that links Poppler and
# libarchive (and cairo, FreeType and fontconfig, which come with Poppler); lord-server never does.
DOCHELPER ?= $(shell pkg-config --exists poppler-glib libarchive cairo-pdf cairo-ft freetype2 fontconfig 2>/dev/null && echo 1 || echo 0)
ifeq ($(DOCHELPER),1)
ALL += build/lord-doc
endif
# lord-netperm (src/netperm, docs/FIREWALL.md step 2): the root service that applies per-app network
# permissions with cgroup BPF programs. The programs are compiled with clang -target bpf (BPF=1,
# if clang is available) and go inside the binary (build/bpf2c → build/gen/np_bpf.h). Without clang it still
# builds and the service warns that it has no BPF. No libbpf or bcc.
.DEFAULT_GOAL := all
BPF ?= $(shell command -v clang >/dev/null 2>&1 && echo 1 || echo 0)
NP_SRC := src/netperm/netperm.c src/netperm/bpfsys.c src/netperm/npcompile.c src/netperm/npdns.c src/netperm/npdiag.c \
          src/fw/rules.c src/fw/dnsmap.c src/fw/fwmap.c src/fw/fwlists.c src/sec/json.c
ALL += build/lord-netperm
ifeq ($(BPF),1)
NP_GEN := build/gen/np_bpf.h
NP_CPP := -DNP_HAVE_BPF
endif
build/bpf2c: src/netperm/bpf2c.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ $<
build/np.bpf.o: src/netperm/np.bpf.c src/netperm/np_shared.h
	@mkdir -p build
	clang -target bpf -O2 -Wall -Wextra -Isrc -I/usr/include/$(shell $(CC) -dumpmachine) -c -o $@ $<
build/gen/np_bpf.h: build/np.bpf.o build/bpf2c
	@mkdir -p build/gen
	build/bpf2c $< $@
build/lord-netperm: $(NP_SRC) $(wildcard src/netperm/*.h) $(wildcard src/fw/*.h) src/sec/json.h $(NP_GEN)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(NP_CPP) $(CFLAGS) $(WARN) -std=c11 -o $@ $(NP_SRC)
# tests/npverify_test.c: the BTF reader, and (as root) every program through the kernel verifier
ifeq ($(BPF),1)
test: build/npverify-test
build/npverify-test: tests/npverify_test.c src/netperm/bpfsys.c $(wildcard src/netperm/*.h) $(NP_GEN)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/npverify_test.c src/netperm/bpfsys.c
	./$@ || { rm -f $@; exit 1; }
endif
test: build/npcompile-test
build/npcompile-test: tests/npcompile_test.c src/netperm/npcompile.c src/netperm/npdns.c src/fw/rules.c src/fw/dnsmap.c src/fw/fwmap.c src/sec/json.c $(wildcard src/netperm/*.h) $(wildcard src/fw/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/npcompile_test.c src/netperm/npcompile.c src/netperm/npdns.c src/fw/rules.c src/fw/dnsmap.c src/fw/fwmap.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
# lord-fand (src/fan, docs/FANS.md): the root service that drives the fans by temperature. libc
# only; it executes nothing and has no network. tests/fan_test.c: curves, fan.json, requests and a
# fake sysfs (run by tests/run_suites.sh).
FAN_SRC := src/fan/fancurve.c src/fan/fanhw.c src/sec/json.c
ALL += build/lord-fand
build/lord-fand: src/fan/fand.c $(FAN_SRC) $(wildcard src/fan/*.h) src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ src/fan/fand.c $(FAN_SRC)
test: build/fan-test
build/fan-test: tests/fan_test.c $(FAN_SRC) $(wildcard src/fan/*.h) src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fan_test.c $(FAN_SRC)
all: $(ALL)

# Headless tests: terminal emulator (tests/vt_dump.c), OpenType shaping against
# HarfBuzz (tests/shape_test.c), bidi against BidiCharacterTest.txt (tests/bidi_test.c) and
# input methods (tests/ime_test.c); Thai line breaking (tests/thbrk_test.c, against libthai
# with tests/thbrk_compare.sh).
#
# Most suites run inside their own build rule (`./$@ || ...`). The ones below are only built here; once
# every prerequisite of `test` is done, tests/run_suites.sh runs them (a few seconds) and fails if any
# fails; a suite whose tool or data isn't installed says "skipped: needs X". `make test-full` adds the
# helpers (lord-media, lord-doc), the sandbox (Landlock kernel) and the HarfBuzz comparison.
test: build/vt-dump build/shape-test build/bidi-test build/ime-test build/thbrk-test build/sound-test build/wasm-test build/net-test build/wasm-js-test build/pkg-test build/fsops-test build/sign-test build/mmdb-test build/canvas-test build/arcpath-test build/ipp-test build/tp-test build/media-test
	@sh tests/run_suites.sh
test-full: test build/sandbox-test build/doc-test $(filter build/lord-media build/lord-doc,$(ALL))
	@sh tests/run_suites.sh full
SHAPE_SRC := src/ui/otf.c src/ui/shape.c src/ui/shape_indic.c src/ui/shape_syl.c src/ui/shape_khmer.c src/ui/shape_myanmar.c src/ui/shape_use.c src/ui/unicode.c
build/shape-test: tests/shape_test.c $(SHAPE_SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/shape_test.c $(SHAPE_SRC)
WASM_SRC := src/wasm/wasm.c src/wasm/wasm_run.c
build/wasm-js-test: tests/wasm_js_test.c src/ui/js_wasm.c src/ui/js_wasm.h $(WASM_SRC) src/wasm/wasm.h $(QJS_OBJ)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/wasm_js_test.c src/ui/js_wasm.c $(WASM_SRC) $(QJS_OBJ) -lm -lpthread
build/wasm-test: tests/wasm_test.c $(WASM_SRC) src/wasm/wasm.h src/wasm/wasm_int.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/wasm_test.c $(WASM_SRC) -lm
PKG_SRC := src/pkg/crypto.c src/pkg/pgp.c src/pkg/xz.c src/pkg/deb.c src/pkg/idx.c src/pkg/pin.c src/pkg/status.c src/pkg/resolve.c src/pkg/http.c src/pkg/pkg.c src/sec/json.c
build/pkg-test: tests/pkg_test.c $(PKG_SRC) src/sec/cgroup.c build/obj/ui/stb.o src/admin.h $(wildcard src/pkg/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/pkg_test.c $(PKG_SRC) src/sec/cgroup.c build/obj/ui/stb.o -lm
PKG_READ_SRC := src/pkg/deb.c src/pkg/idx.c src/pkg/pin.c src/pkg/status.c src/pkg/resolve.c src/pkg/crypto.c
build/pkg-js-test: tests/pkg_js_test.c src/ui/js_pkg.c $(PKG_READ_SRC) $(QJS_OBJ) $(wildcard src/pkg/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/pkg_js_test.c src/ui/js_pkg.c $(PKG_READ_SRC) $(QJS_OBJ) -lm -lpthread
build/thbrk-test: tests/thbrk_test.c src/ui/thbrk.c src/ui/thbrk.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/thbrk_test.c src/ui/thbrk.c
# package signing: SHA-256 (NIST), Ed25519 (RFC 8032), JSON, contenido.json and catalog
build/pkgstate-test: tests/pkgstate_test.c src/sec/pkgstate.c src/sec/sign.c src/sec/json.c src/lpk.c src/pkg/crypto.c build/obj/ui/stb.o $(wildcard src/sec/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/pkgstate_test.c src/sec/pkgstate.c src/sec/sign.c src/sec/json.c src/lpk.c src/pkg/crypto.c build/obj/ui/stb.o -lm
build/sandbox-test: tests/sandbox_test.c src/sec/sandbox.c src/sec/cgroup.c src/sec/sandbox.h src/sec/cgroup.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/sandbox_test.c src/sec/sandbox.c src/sec/cgroup.c -lpthread
build/sign-test: tests/sign_test.c src/sec/json.c src/sec/sign.c src/lpk.c src/pkg/crypto.c build/obj/ui/stb.o $(wildcard src/sec/*.h) src/lpk.h src/pkg/crypto.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -DLORD_SIGN_TEST -o $@ tests/sign_test.c src/sec/json.c src/sec/sign.c src/lpk.c src/pkg/crypto.c build/obj/ui/stb.o -lm
# Verified system code: manifest parser, signature, per-file check, reading under the root (docs/SIGNING.md)
test: build/codesig-test
build/codesig-test: tests/codesig_test.c src/sec/codesig.c src/sec/json.c src/pkg/crypto.c src/sec/codesig.h src/sec/json.h src/pkg/crypto.h build/gen/codesig_build.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/codesig_test.c src/sec/codesig.c src/sec/json.c src/pkg/crypto.c -lpthread
	./$@ ../tools/release/lordos-release.pub.pem || { rm -f $@; exit 1; }
# Clock time zones: TZif and POSIX rules against glibc (docs/SYSTEM.md)
# tests/xwm_test.c: the X11 wire helpers of the window manager (src/wl/x11wire.c, docs/WAYLAND.md)
test: build/xwm-test
build/xwm-test: tests/xwm_test.c src/wl/x11wire.c src/wl/x11wire.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/xwm_test.c src/wl/x11wire.c
	./$@ || { rm -f $@; exit 1; }
# tests/timer_order_test.c: the JS timers (src/ui/js_prelude.h) against the prelude they replaced
test: build/timer-order-test
build/timer-order-test: tests/timer_order_test.c src/ui/js_prelude.h $(QJS_OBJ)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/timer_order_test.c $(QJS_OBJ) -lm -lpthread
	./$@ || { rm -f $@; exit 1; }
test: build/tzif-test
build/tzif-test: tests/tzif_test.c src/ui/tzif.c src/ui/tzif.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/tzif_test.c src/ui/tzif.c
	./$@ || { rm -f $@; exit 1; }
# remote desktop security (docs/REMOTE.md, "Security analysis (0.8.3)"): the strict HTTP, form,
# cookie, WebSocket and JSON parsers, addresses and networks, the failed-login lockout and the
# connection rate; then the same parsers under ASan+UBSan with many random and mutated inputs
REMOTE_SEC_SRC := src/remote/rsec.c src/remote/ipnet.c src/remote/lockout.c src/remote/rtcwire.c
test: build/remote-sec-test build/remote-fuzz
build/remote-sec-test: tests/remote_sec_test.c $(REMOTE_SEC_SRC) src/remote/rsec.h src/remote/ipnet.h src/remote/lockout.h src/remote/rtcwire.h tests/rtc_offer_chrome.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/remote_sec_test.c $(REMOTE_SEC_SRC)
	./$@ || { rm -f $@; exit 1; }
build/remote-fuzz: tests/remote_fuzz.c $(REMOTE_SEC_SRC) src/remote/rsec.h src/remote/ipnet.h src/remote/lockout.h src/remote/rtcwire.h tests/rtc_offer_chrome.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -Itests -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ tests/remote_fuzz.c $(REMOTE_SEC_SRC)
	./$@ 100000 || { rm -f $@; exit 1; }
# journal reader (Logs): normal and compact synthetic files, LZ4 and XZ, broken and
# truncated files; `build/journal-test dump <file>` dumps JSON to compare with journalctl
test: build/journal-test
build/journal-test: tests/journal_test.c src/logs/journal.c src/logs/journal.h src/pkg/xz.c build/obj/ui/stb.o
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/journal_test.c src/logs/journal.c src/pkg/xz.c build/obj/ui/stb.o -lm
	./$@ || { rm -f $@; exit 1; }
# per-app firewall: rules (precedence, networks, JSON) and DNS names (broken packets, LRU)
test: build/fw-rules-test build/dnsmap-test build/broker-restrict-test
# the firewall's non-network restrictions in the real broker code (docs/FIREWALL.md); open() is
# wrapped so a fake /dev/video9 can be opened
BROKER_RESTRICT_SRC := src/sec/brokersrv.c src/sec/brokercam.c src/sec/restrict.c src/sec/perms.c src/sec/json.c \
                       src/fw/rules.c src/fw/fwmap.c src/media/proto.c
build/broker-restrict-test: tests/broker_restrict_test.c $(BROKER_RESTRICT_SRC) $(wildcard src/sec/*.h src/fw/*.h src/media/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/broker_restrict_test.c $(BROKER_RESTRICT_SRC) -Wl,--wrap=open
	./$@ || { rm -f $@; exit 1; }
# lord-maild for the isolated system apps (src/sec/brokermail.c): who may ask what, files, Mail-only verbs
test: build/brokermail-test
build/brokermail-test: tests/brokermail_test.c src/sec/brokermail.c src/sec/json.c $(wildcard src/sec/*.h src/mail/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/brokermail_test.c src/sec/brokermail.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
# tests/launch_cli.c: the real Linux app launcher outside the desktop, for tests/fwlinux/files.sh
# (built by `make test`, run by that script in the VM)
LAUNCH_CLI_SRC := src/apps.c src/fsops.c src/hw/defaults.c src/hw/mimeapps.c src/sec/cgroup.c src/sec/sandbox.c \
                  src/sec/restrict.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
test: build/launch-cli
build/launch-cli: tests/launch_cli.c $(LAUNCH_CLI_SRC) src/apps.h $(wildcard src/sec/*.h src/fw/*.h src/hw/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/launch_cli.c $(LAUNCH_CLI_SRC)
# tests/apps_actions_test.c: desktop actions (Actions= and [Desktop Action …]) in the real launcher
test: build/apps-actions-test
build/apps-actions-test: tests/apps_actions_test.c $(LAUNCH_CLI_SRC) src/apps.h $(wildcard src/sec/*.h src/fw/*.h src/hw/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/apps_actions_test.c $(LAUNCH_CLI_SRC)
	./$@ || { rm -f $@; exit 1; }
# tests/notifyd_cli.c: the real notification server on a private bus, for tests/fwlinux/notify.sh
NOTIFYD_CLI_SRC := src/hw/notifyd.c src/sec/appid.c src/hw/jbuf.c src/vault/dbus.c src/sec/restrict.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
test: build/notifyd-cli build/notifyd-test
build/notifyd-test: tests/notifyd_test.c $(NOTIFYD_CLI_SRC) $(wildcard src/hw/*.h src/sec/*.h src/fw/*.h src/vault/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/notifyd_test.c $(NOTIFYD_CLI_SRC)
	./$@ || { rm -f $@; exit 1; }
build/notifyd-cli: tests/notifyd_cli.c $(NOTIFYD_CLI_SRC) $(wildcard src/hw/*.h src/sec/*.h src/fw/*.h src/vault/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/notifyd_cli.c $(NOTIFYD_CLI_SRC)
# tests/pwguard_cli.c: the PipeWire guard against the session's PipeWire, for tests/fwlinux/mic.sh;
# tests/pwguard_test.c: its POD reader and node classes
PWGUARD_SRC := src/sec/pwguard.c src/sec/appid.c src/sec/restrict.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
test: build/pwguard-cli build/pwguard-test
build/pwguard-cli: tests/pwguard_cli.c $(PWGUARD_SRC) $(wildcard src/sec/*.h src/fw/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/pwguard_cli.c $(PWGUARD_SRC)
build/pwguard-test: tests/pwguard_test.c $(PWGUARD_SRC) $(wildcard src/sec/*.h src/fw/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/pwguard_test.c $(PWGUARD_SRC)
	./$@ || { rm -f $@; exit 1; }
# tests/wl_globals.c: the compositor's globals as a client sees them (tests/fwlinux/screen.sh)
build/wl-globals: tests/wl_globals.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ $< -lwayland-client
build/fw-rules-test: tests/fw_rules_test.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c $(wildcard src/fw/*.h) src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fw_rules_test.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
# subscribed blocklists (0.8.5): raw parsers, the set, the compiled file, fuzzing (docs/FIREWALL.md, "Blocklists")
test: build/fwlists-test
build/fwlists-test: tests/fwlists_test.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c $(wildcard src/fw/*.h) src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fwlists_test.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
build/fwlists-test-asan: tests/fwlists_test.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c $(wildcard src/fw/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ tests/fwlists_test.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
	./$@
# the destinations a user types in the rule sheet (tests/fw_dest_cases.txt), through the store's
# parser; tests/fwrule_input_test.mjs checks the same file against sdk/fwrule.js (run_suites.sh)
test: build/fwdest-test
build/fwdest-test: tests/fwdest_test.c tests/fw_dest_cases.txt tests/fw_domain_cases.txt src/fw/rules.c src/fw/fwmap.c src/sec/json.c $(wildcard src/fw/*.h) src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fwdest_test.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
build/dnsmap-test: tests/dnsmap_test.c src/fw/dnsmap.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c $(wildcard src/fw/*.h) src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/dnsmap_test.c src/fw/dnsmap.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
# backup engine: snapshots, hard links, restore, retention, interruptions, exclusions, encryption
build/lord-server: build/obj/vault/monocypher.o
test: build/backup-test
build/backup-test: tests/backup_test.c src/backup/engine.c src/backup/crypt.c src/fsops.c src/backup/engine.h src/backup/crypt.h src/fsops.h build/obj/vault/monocypher.o
	@mkdir -p build
	$(CC) $(CPPFLAGS) -isystem ../third_party/monocypher $(CFLAGS) $(WARN) -std=c11 -o $@ tests/backup_test.c src/backup/engine.c src/backup/crypt.c src/fsops.c build/obj/vault/monocypher.o -lpthread
	./$@ || { rm -f $@; exit 1; }
# Monitor's MaxMind DB reader (country of an IP): known lookups and broken files
build/mmdb-test: tests/mmdb_test.c src/monitor/mmdb.c src/monitor/mmdb.h src/pkg/xz.c build/obj/ui/stb.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/mmdb_test.c src/monitor/mmdb.c src/pkg/xz.c build/obj/ui/stb.o -lm
# drawing time of the Monitor's vector map (not a test: it measures; see docs/MONITOR.md)
build/map-bench: tests/map_bench.c src/monitor/map.c src/monitor/map.h src/monitor/mon.c src/monitor/disktemp.c src/monitor/conn.c src/monitor/procctl.c src/monitor/mmdb.c src/pkg/xz.c build/obj/ui/stb.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/map_bench.c src/monitor/map.c src/monitor/mon.c src/monitor/disktemp.c src/monitor/conn.c src/monitor/procctl.c src/monitor/mmdb.c src/pkg/xz.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c src/sec/cgroup.c build/obj/ui/stb.o -lm -lpthread
# Monitor process actions (src/monitor/procctl.c): the protected list, stat parsing, a signal that
# refuses a stale start time (PID reuse), TERM/KILL on a child, and the Get Info JSON
test: build/procctl-test
# one reading of Monitor → Network's sockets (tests/conn_dump.c), for checking conn.c by hand
build/conn-dump: tests/conn_dump.c src/monitor/conn.c src/monitor/mon.c src/monitor/disktemp.c src/monitor/mmdb.c src/pkg/xz.c build/obj/ui/stb.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/conn_dump.c src/monitor/mon.c src/monitor/disktemp.c src/monitor/conn.c src/monitor/procctl.c src/monitor/mmdb.c src/pkg/xz.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c src/sec/cgroup.c build/obj/ui/stb.o -lm -lpthread
build/procctl-test: tests/procctl_test.c src/monitor/procctl.c src/monitor/procctl.h src/monitor/mon.c src/monitor/disktemp.c src/monitor/conn.c src/monitor/mmdb.c src/pkg/xz.c build/obj/ui/stb.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/procctl_test.c src/monitor/procctl.c src/monitor/mon.c src/monitor/disktemp.c src/monitor/conn.c src/monitor/mmdb.c src/pkg/xz.c src/fw/rules.c src/fw/fwmap.c src/sec/json.c src/sec/cgroup.c build/obj/ui/stb.o -lm -lpthread
	./$@ || { rm -f $@; exit 1; }
# the Monitor's traffic history file: totals, 7-day/30-day rotation, cut and damaged files, Clear
test: build/nethist-test
build/nethist-test: tests/nethist_test.c src/monitor/nethist.c src/monitor/nethist.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/nethist_test.c src/monitor/nethist.c -lm
	./$@ || { rm -f $@; exit 1; }
# canvas.c rows: SIMD (NEON on aarch64) against C, bit for bit, and how long each one takes
# UI engine (docs/DESIGN.md "Engine notes"): ellipsis in LTR/RTL/Arabic/Thai/emoji, per-side borders,
# per-corner radii, logical edges, transforms and hit testing, and the dragged window's frames against
# the golden hashes in tests/drag_golden_{cpu,gpu}.txt; `build/engine-test bench` times a 2000-row
# list and a window drag on one and two displays
ENGINE_SRC := src/ui/node.c src/ui/style.c src/ui/layout.c src/ui/transform.c src/ui/text.c src/ui/textlayout.c src/ui/image.c \
              src/ui/paint.c src/ui/otf.c src/ui/fonts.c src/ui/unicode.c src/ui/bidi.c src/ui/shape.c src/ui/shape_indic.c \
              src/ui/shape_syl.c src/ui/shape_khmer.c src/ui/shape_myanmar.c src/ui/shape_use.c src/ui/thbrk.c src/canvas.c
test: build/engine-test
build/engine-test: tests/engine_test.c $(ENGINE_SRC) src/outputs.c src/bench/lbinput.c build/obj/ui/stb.o $(HDR) tests/drag_golden_cpu.txt tests/drag_golden_gpu.txt
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/engine_test.c $(ENGINE_SRC) src/outputs.c src/bench/lbinput.c build/obj/ui/stb.o -lm -lpthread
	./$@ || { rm -f $@; exit 1; }
# test_drag_exact's golden hashes for this architecture: only on the reference code (001633b plus
# step 9), never to make a changed frame pass (tests/engine_test.c, "The dragged window, pixel for pixel")
drag-golden: tests/engine_test.c $(ENGINE_SRC) src/outputs.c src/bench/lbinput.c build/obj/ui/stb.o $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o build/engine-golden tests/engine_test.c $(ENGINE_SRC) src/outputs.c src/bench/lbinput.c build/obj/ui/stb.o -lm -lpthread
	./build/engine-golden drag-golden
build/canvas-test: tests/canvas_test.c src/canvas.c src/canvas.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -DLORD_CANVAS_TEST -o $@ tests/canvas_test.c src/canvas.c -lm
build/fsops-test: tests/fsops_test.c src/fsops.c src/fsops.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fsops_test.c src/fsops.c -lpthread
# touchpad driver (src/tp): gestures of the simulated touchpad (../tests/touchpad-sim) → what's recognized
build/tp-test: tests/tp_test.c src/tp/tp.c src/tp/tp.h ../tests/touchpad-sim/gestures.c ../tests/touchpad-sim/gestures.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/tp_test.c src/tp/tp.c ../tests/touchpad-sim/gestures.c -lm
build/ime-test: tests/ime_test.c src/ime.c src/ime.h src/keyboard.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/ime_test.c src/ime.c
build/bidi-test: tests/bidi_test.c src/ui/bidi.c src/ui/unicode.c $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/bidi_test.c src/ui/bidi.c src/ui/unicode.c
build/sound-test: tests/sound_test.c src/sound/pulse.c src/sound/pulse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/sound_test.c src/sound/pulse.c
build/sound-cli: tests/sound_cli.c src/sound/snd.c src/sound/pulse.c src/sound/snd.h src/sound/pulse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/sound_cli.c src/sound/snd.c src/sound/pulse.c -lm
NET_SRC := src/net/nl.c src/net/crypto.c src/net/state.c src/net/sys.c src/net/dhcp.c src/net/wifi.c src/net/wg.c src/net/ovpn.c src/net/vpn.c src/net/vici.c src/net/ipsec.c src/net/vpnproc.c src/net/actions.c
build/net-test: tests/net_test.c $(NET_SRC) $(PKG_SRC) src/sec/cgroup.c build/obj/ui/stb.o src/net/net.h src/net/netpriv.h src/net/vpn.h src/admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/net_test.c $(NET_SRC) $(PKG_SRC) src/sec/cgroup.c build/obj/ui/stb.o -lm
# Test Wayland app with dmabuf (udmabuf): see docs/GPU.md. Needs libwayland-client.
CLIENT_PROTO := stable/xdg-shell/xdg-shell unstable/xdg-decoration/xdg-decoration-unstable-v1 stable/linux-dmabuf/linux-dmabuf-v1 stable/presentation-time/presentation-time \
                unstable/relative-pointer/relative-pointer-unstable-v1 unstable/pointer-constraints/pointer-constraints-unstable-v1 \
                unstable/pointer-gestures/pointer-gestures-unstable-v1 unstable/primary-selection/primary-selection-unstable-v1 \
                stable/viewporter/viewporter staging/fractional-scale/fractional-scale-v1 unstable/xdg-output/xdg-output-unstable-v1 \
                stable/tablet/tablet-v2 staging/cursor-shape/cursor-shape-v1 unstable/idle-inhibit/idle-inhibit-unstable-v1
CLIENT_GEN := $(foreach p,$(CLIENT_PROTO),build/gen/client/$(notdir $(p))-client-protocol.h build/gen/client/$(notdir $(p))-protocol.c)
define wl_client_proto
build/gen/client/$(notdir $(1))-client-protocol.h: $(WLPROTO)/$(1).xml
	@mkdir -p build/gen/client
	wayland-scanner client-header $$< $$@
build/gen/client/$(notdir $(1))-protocol.c: $(WLPROTO)/$(1).xml
	@mkdir -p build/gen/client
	wayland-scanner private-code $$< $$@
endef
$(foreach p,$(CLIENT_PROTO),$(eval $(call wl_client_proto,$(p))))
build/dmabuf-client: tests/dmabuf_client.c $(CLIENT_GEN)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Ibuild/gen $(WARN) -std=c11 -o $@ tests/dmabuf_client.c $(filter %.c,$(CLIENT_GEN)) -lwayland-client
# Who draws the frame: xdg-decoration and KDE's server-decoration modes, also switched live
# (tests/deco_client.c; run inside a session: WAYLAND_DISPLAY=wayland-0 build/deco-client)
build/deco-client: tests/deco_client.c $(CLIENT_GEN)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Ibuild/gen $(WARN) -std=c11 -o $@ tests/deco_client.c $(filter %.c,$(CLIENT_GEN)) -lwayland-client

# Clipboard and primary-selection offers bound to their selection (tests/xwayland/clip_offers.sh, docs/WAYLAND.md)
build/clip-offers: tests/clip_offers.c $(CLIENT_GEN)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Ibuild/gen $(WARN) -std=c11 -o $@ tests/clip_offers.c $(filter %.c,$(CLIENT_GEN)) -lwayland-client
# off/on-monitor cycles against one session (tests/gpu_cycle.sh, docs/GPU.md); a test tool, never installed
build/seat-cycle: tests/seat_cycle.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/seat_cycle.c

MEDIA_PKGS := libavcodec libavformat libavutil libswscale libswresample
MEDIA_VA := $(shell pkg-config --exists libva libva-drm 2>/dev/null && echo 1 || echo 0)
ifeq ($(MEDIA_VA),1)
MEDIA_PKGS += libva libva-drm
MEDIA_CPPFLAGS := -DLM_VAAPI
endif
LM_SRC := $(wildcard src/media/helper/*.c) src/media/proto.c src/sec/sandbox.c src/sec/cgroup.c
build/lord-media: $(LM_SRC) $(wildcard src/media/helper/*.h) src/media/proto.h src/media/aring.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(MEDIA_CPPFLAGS) $(shell pkg-config --cflags $(MEDIA_PKGS)) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LM_SRC) \
	    $(shell pkg-config --libs $(MEDIA_PKGS)) -lpthread -lm
MEDIA_TEST_SRC := tests/media_test.c src/media/proto.c src/media/maudio.c src/media/mpulse.c src/sound/pulse.c
build/media-test: $(MEDIA_TEST_SRC) src/media/proto.h src/media/aring.h src/media/media_int.h src/media/mpulse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(MEDIA_TEST_SRC) -lm

build/vt-dump: tests/vt_dump.c build/obj/term/vt.o build/obj/ui/stb.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $^ $(LDLIBS)

build/gen/%-protocol.h: $(WLPROTO)/stable/xdg-shell/%.xml
	@mkdir -p build/gen
	wayland-scanner server-header $< $@
build/gen/%-protocol.c: $(WLPROTO)/stable/xdg-shell/%.xml
	@mkdir -p build/gen
	wayland-scanner private-code $< $@
build/gen/xdg-decoration-unstable-v1-protocol.h: $(WLPROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	@mkdir -p build/gen
	wayland-scanner server-header $< $@
build/gen/xdg-decoration-unstable-v1-protocol.c: $(WLPROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	@mkdir -p build/gen
	wayland-scanner private-code $< $@
define wl_proto
build/gen/$(notdir $(1))-protocol.h: $(WLPROTO)/$(1).xml
	@mkdir -p build/gen
	wayland-scanner server-header $$< $$@
build/gen/$(notdir $(1))-protocol.c: $(WLPROTO)/$(1).xml
	@mkdir -p build/gen
	wayland-scanner private-code $$< $$@
endef
$(foreach p,$(WL_MORE),$(eval $(call wl_proto,$(p))))

build/gen/%.o: build/gen/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<

build/lord-server: $(OBJ) build/obj/ui/stb.o $(QJS_OBJ) $(WL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

SESSIOND_OBJ := build/obj/remote/sessiond.o build/obj/remote/sess.o build/obj/remote/monitor.o build/obj/remote/conn.o \
                build/obj/remote/enc.o build/obj/remote/pages.o build/obj/sec/cgroup.o build/obj/remote/netsup.o build/obj/remote/cgsup.o \
                build/obj/remote/rsec.o build/obj/remote/ipnet.o build/obj/remote/lockout.o build/obj/remote/rtcwire.o build/obj/remote/rtc.o \
                build/obj/bench/boottest.o
# The remote desktop shows the screen only over WebRTC (docs/REMOTE.md, "Video (WebRTC)"): our own peer in
# the bridge, on Debian's libsrtp2 and usrsctp (and OpenSSL's DTLS). lord-sessiond can't be built without
# them (libsrtp2-dev, libusrsctp-dev).
ifeq ($(SESSIOND),1)
ifneq ($(shell pkg-config --exists libsrtp2 usrsctp 2>/dev/null && echo 1),1)
$(error lord-sessiond needs libsrtp2 and usrsctp for the remote desktop: apt-get install libsrtp2-dev libusrsctp-dev (or SESSIOND=0))
endif
SESSIOND_LIBS := $(shell pkg-config --libs libsrtp2 usrsctp)
build/obj/remote/rtc.o: CPPFLAGS += $(shell pkg-config --cflags libsrtp2 usrsctp)
endif
build/obj/remote/pages.o: src/remote/login.html src/remote/client.html
	@mkdir -p $(dir $@)
	$(LD) -r -b binary -z noexecstack -o $@ src/remote/login.html src/remote/client.html
# Root over SSH (advanced, src/remote/ssh_root.c): public key lines and fingerprints.
test: build/ssh-root-test
build/ssh-root-test: tests/ssh_root_test.c src/remote/ssh_root.c src/remote/ssh_root.h src/pkg/crypto.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/ssh_root_test.c src/remote/ssh_root.c src/pkg/crypto.c
	./$@ || { rm -f $@; exit 1; }
build/lord-sessiond: $(SESSIOND_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(shell pkg-config --libs openssl) $(SESSIOND_LIBS) -lcrypt -lpthread

# Version, channel and build of LordOS, shown in About and Settings → General (lord.version and
# lord.system.info()). Version and channel come from ../package.json ("version" and "lordos": {"channel"}),
# the only place to set them; the build is the date (SOURCE_DATE_EPOCH if set) and the git commit, or
# LORD_BUILD=... The header is rewritten only when its contents change.
LORD_VERSION := $(shell sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' ../package.json 2>/dev/null | head -1 | tr -cd 'A-Za-z0-9.+~-')
LORD_CHANNEL := $(shell sed -n 's/.*"channel": *"\([^"]*\)".*/\1/p' ../package.json 2>/dev/null | head -1 | tr -cd 'a-z0-9-')
LORD_BUILD ?= $(shell d=$$(date -u -d "@$${SOURCE_DATE_EPOCH:-$$(date +%s)}" +%Y%m%d 2>/dev/null || date -u +%Y%m%d); \
                      c=$$(git -C .. rev-parse --short=8 HEAD 2>/dev/null); echo "$$d$${c:+.$$c}")
build/gen/lord_version.h: FORCE
	@mkdir -p build/gen
	@printf '#define LORD_VERSION "%s"\n#define LORD_CHANNEL "%s"\n#define LORD_BUILD "%s"\n' \
	    '$(or $(LORD_VERSION),0.0.0)' '$(LORD_CHANNEL)' '$(shell printf %s '$(LORD_BUILD)' | tr -cd 'A-Za-z0-9.+~-')' > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
build/obj/ui/js.o build/obj/ui/js_sys.o build/obj/ui/js_hw.o build/obj/term/pty.o build/obj/remote/sessiond.o: build/gen/lord_version.h
# Verified system code (src/sec/codesig.c, docs/SIGNING.md "Verified system code"). RELEASE=1: a
# release build, which only runs a manifest signed with the LordOS release key (no development mode).
# CODE_PUBKEY=<64 hex>: a TEST build that trusts a throwaway key instead of the release key (VM tests
# only; never for a release, and refused together with RELEASE=1).
RELEASE ?= 0
CODE_PUBKEY ?=
ifneq ($(and $(filter 1,$(RELEASE)),$(CODE_PUBKEY)),)
$(error CODE_PUBKEY is for test builds only, not with RELEASE=1)
endif
build/gen/codesig_build.h: FORCE
	@mkdir -p build/gen
	@{ [ '$(RELEASE)' = 1 ] && echo '#define LORD_CODE_RELEASE 1'; 	   k=$$(printf %s '$(CODE_PUBKEY)' | tr -cd '0-9a-f'); [ -n "$$k" ] && printf '#define LORD_CODE_PUBKEY_HEX "%s"\n' "$$k"; true; } > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
build/obj/sec/codesig.o: build/gen/codesig_build.h
.PHONY: FORCE
FORCE:

build/obj/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -c -o $@ $<

# The CPU path's pixel loops (blur, rounded rectangles, blending) vectorize at -O3: the backdrop blur
# 9.6 → 5.3 ms and rounded rectangles 1.45× faster on x86-64, same pixels (a later -O3 wins over -O2)
build/obj/canvas.o: CFLAGS += -O3

# Third-party code: compiled as is, without our warnings.
build/obj/ui/stb.o: src/ui/stb.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<

build/qjs/%.o: $(QJS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DCONFIG_VERSION=\"$$(cat $(QJS_DIR)/VERSION)\" -fwrapv -ffp-contract=off -w -c -o $@ $<

clean:
	rm -rf build

# ---- documents (M9) ----
LD_SRC := src/doc/helper.c src/doc/h_pdf.c src/doc/h_arc.c src/doc/h_font.c src/doc/h_make.c src/doc/wire.c src/doc/arcpath.c \
          src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
LD_PKGS := poppler-glib libarchive cairo-pdf cairo-ft freetype2 fontconfig
build/obj/doc/h_stb.o: src/doc/h_stb.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<
build/lord-doc: $(LD_SRC) build/obj/doc/h_stb.o $(wildcard src/doc/*.h) src/sec/sandbox.h src/sec/cgroup.h src/sec/json.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 $(shell pkg-config --cflags $(LD_PKGS)) -o $@ $(LD_SRC) build/obj/doc/h_stb.o $(shell pkg-config --libs $(LD_PKGS)) -lm
# rules for extracting without leaving the folder (tests/arcpath_test.c)
build/arcpath-test: tests/arcpath_test.c src/doc/arcpath.c src/doc/arcpath.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/arcpath_test.c src/doc/arcpath.c
# IPP encoder/decoder, mDNS and page ranges (tests/ipp_test.c)
# (0.8.5) the components' network connectors (src/sec/netconn.c) and what they need, for the tests
NETCONN_SRC := src/sec/netconn.c src/sec/cgroup.c src/fw/rules.c src/fw/fwmap.c
# the connectors: requests, sockets, the real connector process and its fail-closed start
test: build/netconn-test
build/netconn-test: tests/netconn_test.c $(NETCONN_SRC) src/sec/json.c src/sec/netconn.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/netconn_test.c $(NETCONN_SRC) src/sec/json.c -lpthread
	./$@ || { rm -f $@; exit 1; }
build/ipp-test: tests/ipp_test.c src/print/ipp.c src/print/mdns.c src/print/cups.c src/print/phttp.c src/doc/wire.c src/sec/json.c $(NETCONN_SRC) $(wildcard src/print/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/ipp_test.c src/print/ipp.c src/print/mdns.c src/print/cups.c src/print/phttp.c src/doc/wire.c src/sec/json.c $(NETCONN_SRC) -lpthread
# lord-doc end to end: malicious archives, PDF and the lockdown (needs build/lord-doc)
build/doc-test: tests/doc_test.c src/doc/wire.c src/sec/json.c $(wildcard src/doc/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/doc_test.c src/doc/wire.c src/sec/json.c

# Passwords and the system keyring (docs/PASSWORDS.md): rules in mk/vault*.mk
include $(sort $(wildcard mk/*.mk))

# lord-netd (docs/NETWORK.md, "The network belongs to the system"): admin.c with LORD_NETD (net-* actions
# only) plus networking and the package engine (net-vpn-enable). libc only; lord-sessiond runs it.
all: build/lord-netd
NETD_OBJ := build/obj/netd/admin.o build/obj/netd/netd.o $(NET_SRC:src/%.c=build/obj/%.o) $(PKG_SRC:src/%.c=build/obj/%.o) build/obj/sec/cgroup.o build/obj/ui/stb.o
build/obj/netd/admin.o: src/admin.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLORD_NETD $(CFLAGS) $(WARN) -std=c11 -c -o $@ $<
build/obj/netd/netd.o: src/net/netd.c src/admin.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -c -o $@ $<
build/lord-netd: $(NETD_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

# SMART parsers (ATA and NVMe) of the Disks app: hand-built data, healthy/warning/failing
test: build/smart-test
build/smart-test: tests/smart_test.c src/disk/smart.c src/disk/disk.h src/admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/smart_test.c src/disk/smart.c
	./$@ || { rm -f $@; exit 1; }
# smart-* actions (smartmontools): which device names are accepted, JSON compaction and LOG pieces;
# disk temperatures (src/monitor/disktemp.c) on fake sysfs trees: NVMe, multipath NVMe, drivetemp,
# a card and a virtual disk without a sensor, and chips that must be ignored
test: build/smartctl-test build/disktemp-test
build/smartctl-test: tests/smartctl_test.c src/disk/smartctl.c src/disk/disk.h src/admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/smartctl_test.c src/disk/smartctl.c
	./$@ || { rm -f $@; exit 1; }
build/disktemp-test: tests/disktemp_test.c src/monitor/disktemp.c src/monitor/disktemp.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/disktemp_test.c src/monitor/disktemp.c
	./$@ || { rm -f $@; exit 1; }
# manual test of the disk-* actions (root and real devices; not part of `test`)
DISKCLI_SRC := tests/disk_cli.c src/disk/disk.c src/disk/smart.c src/disk/smartctl.c src/disk/disk_admin.c src/disk/smartctl_admin.c src/pkg/crypto.c \
               src/imager/imgverify.c src/imager/imager_admin.c tests/imager_key.c
build/disk-cli: $(DISKCLI_SRC) src/disk/disk.h src/imager/imager_admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(DISKCLI_SRC)
# user folders (src/users/homedirs.c): languages, user-dirs.dirs, no overwriting, no symlinks
test: build/homedirs-test
build/homedirs-test: tests/homedirs_test.c src/users/homedirs.c src/users/homedirs.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/homedirs_test.c src/users/homedirs.c
	./$@ || { rm -f $@; exit 1; }
# hardware video decoding probe (src/media/hwdec.c, docs/VIDEO.md): virtual GPUs and virtual machines
test: build/hwdec-test
build/hwdec-test: tests/hwdec_test.c src/media/hwdec.c src/media/hwdec.h src/media/media_int.h src/media/proto.c src/media/proto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/hwdec_test.c src/media/hwdec.c src/media/proto.c
	./$@ || { rm -f $@; exit 1; }
