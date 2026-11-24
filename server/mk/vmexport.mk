# Machines' Export (docs/VIRTUALIZATION.md 3.6, "Moving a machine", and 6.4): the `.lordvm` copy's manifest and its
# import check, the OVA's OVF descriptor and .mf, and the tar writer. Part of lord-vm (mk/vm.mk adds VMEXPORT_SRC);
# libc, pkg/crypto.c's SHA-256 and sec/json.c only.
VMEXPORT_SRC := src/vm/vmexport.c

# paths, the manifest round trip, verify against hostile folders (changed bytes, missing and extra files, symlinks,
# FIFOs, hard links), the strict parser and a mutation fuzz pass, the OVF (xmllint when present) and .mf, and the tar
# writer read back by the system's tar (a long name, a 9 GiB sparse member); under ASan and UBSan
test: build/vmexport-test
build/vmexport-test: tests/vmexport_test.c $(VMEXPORT_SRC) src/vm/vmexport.h src/pkg/crypto.c src/pkg/crypto.h src/sec/json.c src/sec/json.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmexport_test.c $(VMEXPORT_SRC) src/pkg/crypto.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
