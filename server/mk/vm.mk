# Machines (docs/VIRTUALIZATION.md, "Phase 1 implementation"): lord-vm, the supervisor of one machine (QEMU, passt,
# QMP, limits, state), and lord-vmdisplay, its picture and input over QEMU's peer-to-peer D-Bus display. libc and the
# shared sandbox, cgroup and JSON code; lord-vmdisplay also links the system's D-Bus library (libsystemd), built only
# when its -dev package is there. lordvm-dev plays lord-server's part for tests and development (not installed).
VM_COMMON := src/vm/machine.c src/vm/vmops.c src/vm/vmagent.c src/vm/vmimport.c src/vm/vmxml.c src/sec/json.c src/doc/wire.c
VM_HDR := $(wildcard src/vm/*.h) src/sec/json.h src/doc/wire.h src/sec/cgroup.h src/sec/sandbox.h
LORDVM_SRC := src/vm/lordvm.c src/vm/qmp.c src/vm/vmsandbox.c $(VM_COMMON) src/sec/cgroup.c src/pkg/crypto.c
# lord-vm talks to logind (the shutdown delay and idle inhibitors, PrepareForShutdown) when the system's D-Bus library is
# there; without it, a host shutdown still saves machines through SIGTERM
LORDVM_LOGIND ?= $(shell pkg-config --exists libsystemd 2>/dev/null && echo 1 || echo 0)
ifeq ($(LORDVM_LOGIND),1)
LORDVM_FLAGS := -DVM_LOGIND $(shell pkg-config --cflags libsystemd)
LORDVM_LIBS := $(shell pkg-config --libs libsystemd)
endif
all: build/lord-vm build/lordos-guest
build/lord-vm: $(LORDVM_SRC) $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 $(LORDVM_FLAGS) -o $@ $(LORDVM_SRC) $(LORDVM_LIBS)
	chmod g-w $@

# lordos-guest: the clipboard helper Machines installs in a Linux guest of this computer's architecture (static: it
# runs on whatever the guest has)
build/lordos-guest: ../tools/guest/lordos-guest.c src/vm/vmagent.c src/vm/vmagent.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O2 $(WARN) -std=gnu11 -static -o $@ ../tools/guest/lordos-guest.c src/vm/vmagent.c

VMDISPLAY ?= $(shell pkg-config --exists libsystemd 2>/dev/null && echo 1 || echo 0)
ifeq ($(VMDISPLAY),1)
VMDISPLAY_SRC := src/vm/vmdisplay.c src/sec/sandbox.c src/sec/cgroup.c
all: build/lord-vmdisplay build/lordvm-dev
build/lord-vmdisplay: $(VMDISPLAY_SRC) $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 $(shell pkg-config --cflags libsystemd) -o $@ $(VMDISPLAY_SRC) $(shell pkg-config --libs libsystemd) -lpthread
	chmod g-w $@
build/lordvm-dev: tests/vm_dev.c src/vm/vmproto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/vm_dev.c -lm
endif

# schema validation, the message checks of vmproto.h, QMP line reading and the key map: no QEMU needed.
# The isolation test (tests/vm_isolation.sh) needs KVM and a guest image and runs on the target machines.
test: build/vm-test build/vmimport-test test-vm-import
# what an import refuses and how it merges (6.22): needs qemu-img, qemu-io and python3, no KVM; skipped without them
.PHONY: test-vm-import
test-vm-import: build/lord-vm
	@if command -v qemu-img >/dev/null && command -v qemu-io >/dev/null && command -v python3 >/dev/null; then sh tests/vm_import_test.sh build; \
	else echo "vm_import_test: qemu-img, qemu-io or python3 isn't installed: skipped"; fi
# the importer's parsers (6.22) over invented fixtures, truncated and mutated, under ASan and UBSan
build/vmimport-test: tests/vmimport_test.c src/vm/vmimport.c src/vm/vmxml.c src/vm/machine.c src/sec/json.c src/doc/wire.c $(VM_HDR) $(wildcard tests/vmimport/*)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmimport_test.c src/vm/vmimport.c src/vm/vmxml.c src/vm/machine.c src/sec/json.c src/doc/wire.c
	./$@ || { rm -f $@; exit 1; }
build/vm-test: tests/vm_test.c src/vm/qmp.c $(VM_COMMON) $(VM_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vm_test.c src/vm/qmp.c $(VM_COMMON)
	./$@ || { rm -f $@; exit 1; }

# lord-server's side: lord.vm (ui/js_vm.c), machine.json reading, and the admin actions vm-enable and vm-kvm
VM_SRV_SRC := src/ui/js_vm.c src/vm/machine.c src/vm/vmops.c src/vm/vm_admin.c src/vm/vmthumb.c src/vm/vmagent.c
VM_SRV_OBJ := $(VM_SRV_SRC:src/%.c=build/obj/%.o)
$(VM_SRV_OBJ): $(VM_HDR) src/ui/js_vm.h src/media/media_int.h src/pkg/pkg.h src/admin.h
build/lord-server: $(VM_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_vm.h
