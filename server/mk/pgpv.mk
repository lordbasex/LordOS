# Machines' systems added by the person (docs/VIRTUALIZATION.md 5.6.1; maintainer's approval 2026-11-22):
# lord-pgpv, the sandboxed checker of their OpenPGP signatures (LordOS's own pkg/pgp.c, libc only; no network, no
# files: three sealed memfds from lord-server), and the address rules of the admin action vm-image-get-user.
LORD_PGPV_SRC := src/vm/lordpgpv.c src/vm/vmsig.c src/pkg/pgp.c src/pkg/crypto.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
build/lord-pgpv: $(LORD_PGPV_SRC) src/vm/vmsig.h src/pkg/pgp.h src/pkg/crypto.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(LORD_PGPV_SRC) -lpthread
	chmod g-w $@
all: build/lord-pgpv

test: build/vmurl-test build/vmsig-test
# vmurl.c and pubhost.c: the addresses, sizes and space vm-image-get-user accepts, with a fuzz pass, under ASan and UBSan
build/vmurl-test: tests/vmurl_test.c src/vm/vmurl.c src/vm/vmurl.h src/vm/vmcatalog.c src/vm/vmcatalog.h src/pkg/pubhost.c src/pkg/pubhost.h src/sec/json.c src/pkg/pgp.c src/pkg/crypto.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmurl_test.c src/vm/vmurl.c src/vm/vmcatalog.c src/pkg/pubhost.c src/sec/json.c src/pkg/pgp.c src/pkg/crypto.c
	./$@ || { rm -f $@; exit 1; }
# vmsig.c with keys and signatures made by the test itself (Ed25519): good, changed, another key, a key block with two
# keys, a wrong fingerprint, a future signature, grafted or badly bound subkeys (pgp_keyring_add_untrusted); lord-server's
# reading of the answer; then lord-pgpv itself on memfds
build/vmsig-test: tests/vmsig_test.c src/vm/vmsig.c src/vm/vmsig.h src/pkg/pgp.c src/pkg/crypto.c build/lord-pgpv
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmsig_test.c src/vm/vmsig.c src/pkg/pgp.c src/pkg/crypto.c
	./$@ build/lord-pgpv || { rm -f $@; exit 1; }
