# Machine groups (docs/VIRTUALIZATION.md 19.6, "Unpacking"; maintainer's approval 2026-11-23): lord-vmunpack, the
# sandboxed helper that reads a verified group archive (Whonix's KVM .libvirt.xz: a GNU tar in xz) on fd 0 and writes
# exactly its two members' disks on fds 3 and 4. libc and LordOS's own xz decoder (pkg/xz.c, built without gzip) only;
# no liblzma, no libarchive. Started by lord-vm, never by root.
LORD_VMUNPACK_SRC := src/vm/vmunpack.c src/pkg/xz.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
build/lord-vmunpack: $(LORD_VMUNPACK_SRC) src/vm/vmunpack.h src/pkg/xz.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DXZ_NO_GZ -o $@ $(LORD_VMUNPACK_SRC) -lpthread
	chmod g-w $@
all: build/lord-vmunpack

# the tar reader and the streaming xz decoder on archives the test makes (good, hostile, damaged, a fuzz pass), then
# lord-vmunpack itself on descriptors; under ASan and UBSan
test: build/vmunpack-test
build/vmunpack-test: tests/vmunpack_test.c src/vm/vmunpack.c src/vm/vmunpack.h src/pkg/xz.c src/pkg/xz.h build/lord-vmunpack
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -DXZ_NO_GZ -DVMU_NO_MAIN -o $@ tests/vmunpack_test.c src/vm/vmunpack.c src/pkg/xz.c
	./$@ build/lord-vmunpack || { rm -f $@; exit 1; }
