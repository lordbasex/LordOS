# Machines (docs/VIRTUALIZATION.md, "Phase 1 implementation"): lord-vm, the supervisor of one machine (QEMU, passt,
# QMP, limits, state), and lord-vmdisplay, its picture and input over QEMU's peer-to-peer D-Bus display. libc and the
# shared sandbox, cgroup and JSON code; lord-vmdisplay also links the system's D-Bus library (libsystemd), built only
# when its -dev package is there. lordvm-dev plays lord-server's part for tests and development (not installed).
VM_COMMON := src/vm/machine.c src/vm/vmgroup.c src/vm/vmlink.c src/vm/vmnet.c src/vm/vmgpu.c src/vm/vmops.c src/vm/vmagent.c src/vm/vmrecv.c src/vm/vmimport.c src/vm/vmxml.c src/sec/json.c src/doc/wire.c
VM_HDR := $(wildcard src/vm/*.h) src/sec/json.h src/doc/wire.h src/sec/cgroup.h src/sec/sandbox.h
LORDVM_SRC := src/vm/lordvm.c src/vm/vmusb.c src/vm/vmiso.c src/vm/vmisoans.c src/vm/vmexec.c src/vm/vmexport.c src/vm/vmlog.c src/vm/vmcpu.c src/vm/qmp.c src/vm/vmpin.c src/vm/vmsandbox.c src/vm/vmseccomp.c src/vm/vmunattend.c src/vm/vmgvtwin.c src/vm/vmperfwin.c $(VM_COMMON) src/sec/cgroup.c src/pkg/crypto.c src/vm/vmcatalog.c src/pkg/pgp.c src/imager/imgverify.c
# lord-vm talks to logind (the shutdown delay and idle inhibitors, PrepareForShutdown) when the system's D-Bus library is
# there; without it, a host shutdown still saves machines through SIGTERM
LORDVM_LOGIND ?= $(shell pkg-config --exists libsystemd 2>/dev/null && echo 1 || echo 0)
ifeq ($(LORDVM_LOGIND),1)
LORDVM_FLAGS := -DVM_LOGIND $(shell pkg-config --cflags libsystemd)
LORDVM_LIBS := $(shell pkg-config --libs libsystemd)
LORDVM_SRC += src/vm/vmlogind.c
endif
# every system call's name on this computer, from the kernel's own header: lord-vm names the call its QEMU allowlist
# (vmseccomp.c) stopped. Made at build time, so it is always this architecture's table
build/gen/vmsysnames.h:
	@mkdir -p build/gen
	echo '#include <asm/unistd.h>' | $(CC) -E -dM -x c - | sed -n 's/^#define __NR_\([a-z0-9_]*\) .*/{ "\1", __NR_\1 },/p' | \
		grep -v -e '"syscalls"' -e '"arch_specific_syscall"' | LC_ALL=C sort >$@.new && [ -s $@.new ] && mv $@.new $@
VM_HDR += build/gen/vmsysnames.h
all: build/lord-vm build/lordos-guest build/lordos-guest-tools.iso
build/lord-vm: $(LORDVM_SRC) $(VM_HDR) build/gen/guesttools.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 $(LORDVM_FLAGS) -o $@ $(LORDVM_SRC) $(LORDVM_LIBS)
	chmod g-w $@

# lord-vm-usb.so (src/vm/vmusbshim.c): preloaded into a machine's QEMU so that its libusb looks for no device of its own
# (docs/VIRTUALIZATION.md 6.6); only dlopen and dlsym of the C library. Installed as /usr/lib/lordos/lib/lord-vm-usb.so
all: build/lord-vm-usb.so
build/lord-vm-usb.so: src/vm/vmusbshim.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O2 $(WARN) -std=gnu11 -shared -fPIC -Wl,-z,relro,-z,now -o $@ src/vm/vmusbshim.c

# vmusb.c: the host's USB devices from a made-up sysfs, the request and rule parsers (fuzzed), the refusals (an input node
# a process holds, a mounted or held disk, storage for someone who isn't an administrator), the descriptor check
test: build/vmusb-test
build/vmusb-test: tests/vmusb_test.c src/vm/vmusb.c src/vm/vmusb.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmusb_test.c src/vm/vmusb.c
	./$@ || { rm -f $@; exit 1; }

# the same lord-vm with LORDVM_TEST_PATHS (lordvm.c): QEMU, qemu-img, passt and the UEFI firmware from absolute paths in
# the environment, for the release gates on a host that keeps them elsewhere. Built only when asked, never installed.
build/lord-vm-testpaths: $(LORDVM_SRC) $(VM_HDR) build/gen/guesttools.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 $(LORDVM_FLAGS) -DLORDVM_TEST_PATHS -o $@ $(LORDVM_SRC) $(LORDVM_LIBS)
	chmod g-w $@
# the suites that run it (vm-group-manage, vm-storage, machines-group-manage in tests/run_suites.sh) must never run a
# stale one: `make test` rebuilds it whenever lord-vm's sources changed
test: build/lord-vm-testpaths

# lordos-guest: the clipboard helper Machines installs in a Linux guest of this computer's architecture (static: it
# runs on whatever the guest has)
build/lordos-guest: ../tools/guest/lordos-guest.c ../tools/guest/gshell.c ../tools/guest/gshell.h ../tools/guest/x11clip.c ../tools/guest/x11clip.h ../tools/guest/perf.c ../tools/guest/perf.h ../tools/guest/dbusmin.c ../tools/guest/dbusmin.h ../tools/guest/gsend.c ../tools/guest/gsend.h ../tools/guest/gnotes.c ../tools/guest/gnotes.h src/vm/vmagent.c src/vm/vmagent.h src/pkg/crypto.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O2 $(WARN) -std=gnu11 -static -o $@ ../tools/guest/lordos-guest.c ../tools/guest/gshell.c ../tools/guest/x11clip.c ../tools/guest/perf.c ../tools/guest/dbusmin.c ../tools/guest/gsend.c ../tools/guest/gnotes.c src/vm/vmagent.c src/pkg/crypto.c

# The "LordOS Guest Tools" disc (docs/VIRTUALIZATION.md 6.17): for a Linux machine without qemu-guest-agent, a read-only
# ISO 9660 image with lordos-guest (stripped), install.sh, its checksum and a README, made here by our own small writer
# (tools/guest/mkguestiso.c: same bytes for the same files). Its SHA-256 is compiled into lord-vm (build/gen/guesttools.h),
# which attaches only that image; the package installs it as /usr/lib/lordos/guest/lordos-guest-tools-<arch>.iso.
GUEST_TOOLS_ARCH ?= $(or $(DEB_ARCH),$(shell dpkg --print-architecture 2>/dev/null || (uname -m | sed -e 's/x86_64/amd64/' -e 's/aarch64/arm64/')))
build/mkguestiso: ../tools/guest/mkguestiso.c
	@mkdir -p build
	$(CC) -O2 $(WARN) -std=gnu11 -o $@ ../tools/guest/mkguestiso.c
build/lordos-guest-tools.iso: build/mkguestiso build/lordos-guest ../tools/guest/install.sh ../tools/guest/guest-tools-readme.txt
	@mkdir -p build/guesttools
	strip --strip-unneeded -o build/guesttools/lguest build/lordos-guest 2>/dev/null || cp build/lordos-guest build/guesttools/lguest
	printf '%s\n' '$(GUEST_TOOLS_ARCH)' > build/guesttools/arch.txt
	cd build/guesttools && sha256sum lguest > sha256.txt
	./build/mkguestiso $@ LORDOS_TOOLS INSTALL.SH=../tools/guest/install.sh LGUEST=build/guesttools/lguest \
		ARCH.TXT=build/guesttools/arch.txt SHA256.TXT=build/guesttools/sha256.txt README.TXT=../tools/guest/guest-tools-readme.txt
# the disc read back with lord-vm's own ISO reader, its hash against guesttools.h, the writer's refusals (ASan, UBSan);
# and install.sh's choices for each package manager, init system and prompt (tests/guest_tools_install_test.sh)
test: build/guesttools-test
build/guesttools-test: tests/guesttools_test.c src/vm/vmunattend.c src/vm/vmunattend.h src/sec/json.c src/pkg/crypto.c build/gen/guesttools.h build/mkguestiso
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/guesttools_test.c src/vm/vmunattend.c src/sec/json.c src/pkg/crypto.c
	./$@ || { rm -f $@; exit 1; }
build/gen/guesttools.h: build/lordos-guest-tools.iso
	@mkdir -p build/gen
	printf '#define GUEST_TOOLS_SHA256 "%s"\n#define GUEST_TOOLS_ARCH "%s"\n' "$$(sha256sum build/lordos-guest-tools.iso | cut -d' ' -f1)" '$(GUEST_TOOLS_ARCH)' > $@.new && mv $@.new $@

# lordos-guest-win.exe: LordOS guest tools for Windows guests (tools/guest-win), cross-built with mingw-w64
# (gcc-mingw-w64-x86-64-posix, docs/VM-SETUP.md): Win32 only, libgcc and winpthread linked in, no DLL of ours. Skipped
# with a note when the compiler isn't there (Machines then can't install the tools into a Windows guest).
MINGW_CC ?= $(shell command -v x86_64-w64-mingw32-gcc-posix 2>/dev/null || command -v x86_64-w64-mingw32-gcc 2>/dev/null)
ifneq ($(MINGW_CC),)
all: build/lordos-guest-win.exe
build/lordos-guest-win.exe: ../tools/guest-win/lordos-guest-win.c ../tools/guest-win/lordos-guest-win.rc ../tools/guest-win/lordos-guest-win.manifest src/vm/vmagent.c src/vm/vmagent.h
	@mkdir -p build
	$(dir $(MINGW_CC))x86_64-w64-mingw32-windres -I ../tools/guest-win ../tools/guest-win/lordos-guest-win.rc -O coff -o build/lordos-guest-win.res
	$(MINGW_CC) -O2 $(WARN) -std=gnu11 -municode -mwindows -static -static-libgcc -s -o $@ ../tools/guest-win/lordos-guest-win.c src/vm/vmagent.c \
		build/lordos-guest-win.res -lole32 -loleaut32 -luuid -lwindowscodecs -ldwmapi -lwtsapi32 -lshell32 -lgdi32 -luser32 -lbcrypt -lpowrprof
else
all: lordos-guest-win-skipped
lordos-guest-win-skipped:
	@echo "skipped: lordos-guest-win.exe needs x86_64-w64-mingw32-gcc-posix (gcc-mingw-w64-x86-64-posix)"
.PHONY: lordos-guest-win-skipped
endif

VMDISPLAY ?= $(shell pkg-config --exists libsystemd 2>/dev/null && echo 1 || echo 0)
ifeq ($(VMDISPLAY),1)
VMDISPLAY_SRC := src/vm/vmdisplay.c src/sec/sandbox.c src/sec/cgroup.c
all: build/lord-vmdisplay build/lordvm-dev
build/lord-vmdisplay: $(VMDISPLAY_SRC) $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 $(shell pkg-config --cflags libsystemd) -o $@ $(VMDISPLAY_SRC) $(shell pkg-config --libs libsystemd) -lpthread
	chmod g-w $@
build/lordvm-dev: tests/vm_dev.c src/vm/vmproto.h src/vm/vmagent.c src/vm/vmagent.h src/vm/vmrecv.c src/vm/vmrecv.h src/media/maudio.c src/media/mpulse.c src/sound/pulse.c src/media/aring.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/vm_dev.c src/vm/vmagent.c src/vm/vmrecv.c src/media/maudio.c src/media/mpulse.c src/sound/pulse.c -lm $(if $(filter 1,$(GPU)),$(shell pkg-config --libs gbm egl glesv2))
# lord-vmdisplay against a mock QEMU (its D-Bus display) and a mock lord-server: QEMU's listener calls answered inside
# its 1 s, and buttons after the pointer's position in both mouse modes. No QEMU, KVM or GPU
test: build/vmdisplay-test
build/vmdisplay-test: tests/vmdisplay_test.c src/vm/vmproto.h src/vm/vmpace.h build/lord-vmdisplay
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 $(shell pkg-config --cflags libsystemd) -o $@ tests/vmdisplay_test.c $(shell pkg-config --libs libsystemd)
	./$@ build/lord-vmdisplay 2>build/vmdisplay-test.err || { cat build/vmdisplay-test.err; rm -f $@; exit 1; }
endif

# schema validation, the message checks of vmproto.h, QMP line reading and the key map: no QEMU needed.
# The isolation test (tests/vm_isolation.sh) needs KVM and a guest image and runs on the target machines.
test: build/vmpace-test build/vmmem-test build/vm-test build/guest-dbus-test build/vmlog-test build/vmgpu-test build/vmimport-test build/vmcatalog-test build/http-resume-test build/http-retry-test build/vmunattend-test build/vmthumb-test build/vmcpu-test test-vm-import
# machine groups (docs/VIRTUALIZATION.md 19.4): group.json's parser, machine.json schema 14's member keys and locks, a
# fuzz pass; under ASan and UBSan
test: build/vmgroup-test
build/vmgroup-test: tests/vmgroup_test.c src/vm/vmgroup.c src/vm/machine.c src/sec/json.c src/doc/wire.c $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmgroup_test.c src/vm/vmgroup.c src/vm/machine.c src/sec/json.c src/doc/wire.c
	./$@ || { rm -f $@; exit 1; }
# a group's link (vm/vmlink.c): VM_LINK's checks, lord-vm's descriptor check, lord-server's pairing against stand-in
# lord-vms; under ASan and UBSan
test: build/vmlink-test
build/vmlink-test: tests/vmlink_test.c src/vm/vmlink.c src/vm/vmgroup.c src/vm/machine.c src/sec/json.c src/doc/wire.c $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmlink_test.c src/vm/vmlink.c src/vm/vmgroup.c src/vm/machine.c src/sec/json.c src/doc/wire.c
	./$@ || { rm -f $@; exit 1; }
# vm-image-list and vm-image-get for a machine group's archive (vmimage_admin.c) with the network replaced, as root of a
# user namespace (unshare -Ur): the release floor and its root-side high-water mark, a list's and an archive's release
# bound to their folder's, the archive only under a temporary name until its SHA-512 matched; under ASan and UBSan
VMIMG_GROUP_SRC := tests/vmimage_group_test.c src/vm/vmimage_admin.c src/vm/vmcatalog.c src/vm/vmurl.c src/pkg/pubhost.c src/pkg/crypto.c src/pkg/pgp.c src/sec/json.c src/imager/imgverify.c tests/imager_key.c
test: build/vmimage-group-test
build/vmimage-group-test: $(VMIMG_GROUP_SRC) src/vm/vmcatalog.h src/vm/vm_admin.h ../assets/machines/systems.json tests/vmimage/whonix.sums tests/vmimage/whonix.asc
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -DVM_CATALOG='"build/vmimage-group-test/systems.json"' -DVM_STORE='"build/vmimage-group-test/machines"' -DVM_KEYS='"build/vmimage-group-test/keys"' -o $@ $(VMIMG_GROUP_SRC)
	unshare -Ur ./$@; s=$$?; [ $$s = 0 ] || [ $$s = 77 ] || { rm -f $@; exit 1; }
# lord-server's side of a group's link (vm/vmlink.c) as a command, for tests/vm_group_link.sh; not installed
all: build/vm-link-tool
build/vm-link-tool: tests/vm_link_tool.c src/vm/vmlink.c src/vm/vmgroup.c src/vm/machine.c src/sec/json.c src/doc/wire.c $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/vm_link_tool.c src/vm/vmlink.c src/vm/vmgroup.c src/vm/machine.c src/sec/json.c src/doc/wire.c
# the download catalog's checks as the admin helper makes them (vmcatalog.c), over the real assets/machines/systems.json
# and invented lists, under ASan and UBSan
# and the makers' signatures on their lists (pkg/pgp.c) with the keys the catalog pins, over the lists of tests/vmimage,
# and LordOS's own release signature (imager/imgverify.c) with the compiled-in release key
VMCAT_SRC := src/vm/vmcatalog.c src/sec/json.c src/pkg/pgp.c src/pkg/crypto.c src/imager/imgverify.c
build/vmcatalog-test: tests/vmcatalog_test.c $(VMCAT_SRC) tests/imager_key.c src/vm/vmcatalog.h src/pkg/pgp.h src/imager/imgverify.h ../assets/machines/systems.json $(wildcard tests/vmimage/*) $(wildcard ../packaging/keys/machines/*)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmcatalog_test.c $(VMCAT_SRC) tests/imager_key.c
	./$@ ../assets/machines/systems.json tests/vmimage ../packaging/keys/machines || { rm -f $@; exit 1; }
# unattended installs (vmunattend.c): the spec, the answer files, the FAT seed disk, the cpio, ISO 9660 paths
build/vmunattend-test: tests/vmunattend_test.c src/vm/vmunattend.c src/vm/vmunattend.h src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmunattend_test.c src/vm/vmunattend.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
# what an ISO boots on (vmiso.c, lord.vm.isoInfo): ISO 9660, El Torito, the ESP's FAT, UDF and PE headers in images
# made by the test, hostile ones (loops, huge sizes) and a fuzz pass of random changes
test: build/vmiso-test
build/vmiso-test: tests/vmiso_test.c src/vm/vmiso.c src/vm/vmisoans.c src/vm/vmiso.h build/lord-vm
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmiso_test.c src/vm/vmiso.c src/vm/vmisoans.c
	./$@ || { rm -f $@; exit 1; }
# resumed downloads (pkg/http.c, FetchItem.resume) against a server on loopback: as the user who runs it
# how a machine's memory is backed (vmmem.h): hugetlb only when it all fits in free 2 MB pages
build/vmmem-test: tests/vmmem_test.c src/vm/vmmem.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmmem_test.c
	./$@ || { rm -f $@; exit 1; }
# how often a machine's picture is taken (vmpace.h, VD_PACE)
build/vmpace-test: tests/vmpace_test.c src/vm/vmpace.h src/vm/vmproto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmpace_test.c
	./$@ || { rm -f $@; exit 1; }
# the card's picture (vmthumb.c): the box filter and the write off the UI thread
build/vmthumb-test: tests/vmthumb_test.c src/vm/vmthumb.c src/vm/vmthumb.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmthumb_test.c src/vm/vmthumb.c -lpthread
	./$@ || { rm -f $@; exit 1; }
# what a machine's vCPUs use (vmcpu.c, the Machines app's vCPU recommendation): the stat line, QEMU's vCPU thread names,
# the windows of five minutes and cpu-usage.json, under ASan and UBSan; no QEMU needed (its own threads stand in)
# VM_EXEC (docs/VIRTUALIZATION.md 6.15): the request, the answer and the guest's printout, the guest's two scripts under
# /bin/sh with stub runuser and loginctl, a fuzz pass
test: build/vmexec-test
build/vmexec-test: tests/vmexec_test.c src/vm/vmexec.c src/vm/vmexec.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmexec_test.c src/vm/vmexec.c
	./$@ || { rm -f $@; exit 1; }
build/vmcpu-test: tests/vmcpu_test.c src/vm/vmcpu.c src/vm/vmcpu.h src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmcpu_test.c src/vm/vmcpu.c src/sec/json.c -lpthread
	./$@ || { rm -f $@; exit 1; }

build/http-resume-test: tests/http_resume_test.c src/pkg/http.c src/pkg/pubhost.c src/pkg/crypto.c src/sec/cgroup.c src/pkg/http.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -DLORD_HTTP_TEST -o $@ tests/http_resume_test.c src/pkg/http.c src/pkg/pubhost.c src/pkg/crypto.c src/sec/cgroup.c
	./$@ || { rm -f $@; exit 1; }
# the request line's '+' and a signed redirect refused with 403/5xx asked for again from the original URL (get_one)
build/http-retry-test: tests/http_retry_test.c src/pkg/http.c src/pkg/pubhost.c src/pkg/crypto.c src/sec/cgroup.c src/pkg/http.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -DLORD_HTTP_TEST -o $@ tests/http_retry_test.c src/pkg/http.c src/pkg/pubhost.c src/pkg/crypto.c src/sec/cgroup.c
	./$@ || { rm -f $@; exit 1; }
# what an import refuses and how it merges (6.22): needs qemu-img, qemu-io and python3, no KVM; skipped without them
.PHONY: test-vm-import
test-vm-import: build/lord-vm
	@if command -v qemu-img >/dev/null && command -v qemu-io >/dev/null && command -v python3 >/dev/null; then sh tests/vm_import_test.sh build; \
	else echo "vm_import_test: qemu-img, qemu-io or python3 isn't installed: skipped"; fi
# Machines' graphics cards (vmgpu.c, vmgpu_admin.c): the parsers, an invented sysfs and the four admin actions under a
# fake root folder (no root, nothing of this computer's /etc or /sys touched), under ASan and UBSan
build/vmgpu-test: tests/vmgpu_test.c src/vm/vmgpu.c src/vm/vmgpu_admin.c src/vm/vmgpu.h src/vm/vm_admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -Isrc -o $@ tests/vmgpu_test.c src/vm/vmgpu.c src/vm/vmgpu_admin.c
	./$@ || { rm -f $@; exit 1; }
# the importer's parsers (6.22) over invented fixtures, truncated and mutated, under ASan and UBSan
build/vmimport-test: tests/vmimport_test.c src/vm/vmimport.c src/vm/vmxml.c src/vm/machine.c src/sec/json.c src/doc/wire.c $(VM_HDR) $(wildcard tests/vmimport/*)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmimport_test.c src/vm/vmimport.c src/vm/vmxml.c src/vm/machine.c src/sec/json.c src/doc/wire.c
	./$@ || { rm -f $@; exit 1; }
# lord-vm's logind side (vmlogind.c) against a fake logind on a private dbus-daemon: PrepareForShutdown handled at once
# (it waited ~30 s on 2026-11-23), the delay inhibitor's descriptor, the bus going away. Skipped without libsystemd's
# -dev package (lord-vm then has no logind side) or, when it runs, without dbus-daemon
ifeq ($(LORDVM_LOGIND),1)
test: build/vmlogind-test
build/vmlogind-test: tests/vmlogind_test.c src/vm/vmlogind.c src/vm/vmlogind.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 $(shell pkg-config --cflags libsystemd) -o $@ tests/vmlogind_test.c src/vm/vmlogind.c $(shell pkg-config --libs libsystemd)
	./$@ || { rm -f $@; exit 1; }
endif
# QEMU's seccomp allowlist (vmseccomp.c): both architectures' rows (the other one's built against its own kernel header),
# the dangerous calls absent, and the BPF program run by an interpreter for every machine option; no filter loaded
test: build/vmseccomp-test
build/vmseccomp-test: tests/vmseccomp_test.c tests/vmseccomp_other.c src/vm/vmseccomp.c src/vm/vmseccomp.h src/vm/vmsyscalls.h build/gen/vmsysnames.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmseccomp_test.c tests/vmseccomp_other.c src/vm/vmseccomp.c
	./$@ || { rm -f $@; exit 1; }
# QEMU's jail as a compromised QEMU finds it (vmsandbox.c's namespaces, private loader, Landlock and seccomp, then
# exec), and what the person's other programs can't do to it; "skipped" without user namespaces or Landlock ABI 4
test: build/vm-jail-probe
build/vm-jail-probe: tests/vm_jail_probe.c src/vm/vmsandbox.c src/vm/vmsandbox.h src/vm/vmproto.h src/vm/vmseccomp.c src/vm/vmseccomp.h src/vm/vmsyscalls.h build/gen/vmsysnames.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -Ibuild/gen -o $@ tests/vm_jail_probe.c src/vm/vmsandbox.c src/vm/vmseccomp.c
	./$@ || { rm -f $@; exit 1; }
build/vm-test: tests/vm_test.c src/vm/qmp.c src/vm/vmgvtwin.c src/vm/vmperfwin.c $(VM_COMMON) $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vm_test.c src/vm/qmp.c src/vm/vmgvtwin.c src/vm/vmperfwin.c $(VM_COMMON)
	./$@ || { rm -f $@; exit 1; }
# lordos-guest's own D-Bus client and the performance profile's record (tools/guest/dbusmin.c, perf.c); the calls on
# real buses are tests/guest_perf_test.sh (run_suites.sh)
build/guest-dbus-test: tests/guest_dbus_test.c ../tools/guest/dbusmin.c ../tools/guest/dbusmin.h ../tools/guest/perf.c ../tools/guest/perf.h src/vm/vmagent.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/guest_dbus_test.c ../tools/guest/dbusmin.c ../tools/guest/perf.c
	./$@ || { rm -f $@; exit 1; }

# lord-server's side: lord.vm (ui/js_vm.c), machine.json reading, and the admin actions vm-enable and vm-kvm
VM_SRV_SRC := src/ui/js_vm.c src/vm/vmkeysrv.c src/vm/vmexport.c src/vm/machine.c src/vm/vmgroup.c src/vm/vmlink.c src/vm/vmops.c src/vm/vm_admin.c src/vm/vmnet.c src/vm/vmnet_admin.c src/vm/vmgpu.c src/vm/vmgpu_admin.c src/vm/vmimage_admin.c src/vm/vmcatalog.c src/vm/vmurl.c src/vm/vmsig.c src/vm/vmthumb.c src/vm/vmagent.c src/vm/vmrecv.c src/vm/vmpin.c src/vm/vmisoans.c src/vm/vmusb.c src/vm/vmusb_admin.c
VM_SRV_OBJ := $(VM_SRV_SRC:src/%.c=build/obj/%.o)
$(VM_SRV_OBJ): $(VM_HDR) src/ui/js_vm.h src/media/media_int.h src/pkg/pkg.h src/admin.h
build/lord-server: $(VM_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_vm.h

# vm-usb-allow like the admin helper runs it, for tests/vm_usb_e2e.sh and manual checks (needs root and a real device: not in
# make test)
build/vm-usb-cli: tests/vm_usb_cli.c src/vm/vmusb_admin.c src/vm/vmusb.c src/vm/vmusb.h src/vm/vm_admin.h src/admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -Isrc -o $@ tests/vm_usb_cli.c src/vm/vmusb_admin.c src/vm/vmusb.c

# vm-image-* like the admin helper runs them, against the makers' servers (tests/vm_image_cli.c; a manual check, not in
# make test: it needs root, lord-tls beside it and the network). Its catalog, keys and store are under /var/tmp/lordos-vmimage-test (root's: sudo install the catalog and packaging/keys/machines there first).
VMIMAGE_CLI_SRC := tests/vm_image_cli.c src/vm/vmimage_admin.c src/vm/vmcatalog.c src/vm/vmurl.c src/pkg/http.c src/pkg/pubhost.c src/pkg/crypto.c src/pkg/pgp.c src/sec/cgroup.c src/sec/json.c src/imager/imgverify.c tests/imager_key.c
build/vm-image-cli: $(VMIMAGE_CLI_SRC) src/vm/vmcatalog.h src/vm/vm_admin.h src/pkg/http.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DVM_CATALOG='"/var/tmp/lordos-vmimage-test/systems.json"' -DVM_STORE='"/var/tmp/lordos-vmimage-test/machines"' -DVM_KEYS='"/var/tmp/lordos-vmimage-test/keys"' -o $@ $(VMIMAGE_CLI_SRC)

# vm-net-bridge and vm-net-unbridge like the admin helper runs them (tests/vm_net_cli.c; a manual check, not in make test:
# it needs root and a wired card, and makes a real macvtap)
VMNET_CLI_SRC := tests/vm_net_cli.c src/vm/vmnet_admin.c src/vm/vmnet.c src/vm/machine.c src/sec/json.c src/doc/wire.c src/net/nl.c
build/vm-net-cli: $(VMNET_CLI_SRC) src/vm/vmnet.h src/vm/vm_admin.h src/net/netpriv.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(VMNET_CLI_SRC)

# the same, but a scratch dummy interface named lordtest<N> counts as a wired card (tests/vm_net_dummy_wrap.c, vmnet_card
# wrapped at link time): the Bridged run of tests/vm_bridge_dummy.sh without the real wired card. Test only.
build/vm-net-cli-dummy: $(VMNET_CLI_SRC) tests/vm_net_dummy_wrap.c src/vm/vmnet.h src/vm/vm_admin.h src/net/netpriv.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -Isrc -Wl,--wrap=vmnet_card -o $@ $(VMNET_CLI_SRC) tests/vm_net_dummy_wrap.c

# vm-gvt-create, vm-gvt-remove and gpu-min-freq like the admin helper runs them (tests/vm_gvt_cli.c; a manual check, not
# in make test: it needs root and an iGPU running GVT-g, and makes a real mdev)
build/vm-gvt-cli: tests/vm_gvt_cli.c src/vm/vmgpu.c src/vm/vmgpu_admin.c src/vm/vmgpu.h src/vm/vm_admin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -Isrc -o $@ tests/vm_gvt_cli.c src/vm/vmgpu.c src/vm/vmgpu_admin.c

# lord-vm's log (src/vm/vmlog.c): a child's output in whole lines, a repeated line counted (QEMU calling a closed
# D-Bus listener every 30 ms filled the log on 2026-11-22), rotation when full; under ASan and UBSan
build/vmlog-test: tests/vmlog_test.c src/vm/vmlog.c src/vm/vmlog.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmlog_test.c src/vm/vmlog.c
	./$@ || { rm -f $@; exit 1; }
# what Sentinela checks of a running machine (vm/vmwatch.h, js_vm.c stats): the scope's exact path, a cgroup "in" it only
# when it is that path or below it (never a path that merely contains it), the installed lord-vm; under ASan and UBSan
test: build/vmwatch-test
build/vmwatch-test: tests/vmwatch_test.c src/vm/vmwatch.h src/vm/vmproto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmwatch_test.c
	./$@ || { rm -f $@; exit 1; }
build/obj/ui/js_vm.o: src/vm/vmwatch.h
