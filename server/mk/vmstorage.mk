# Machines' storage (docs/VIRTUALIZATION.md 6.2, 6.4, 8): lord-server's side of encrypted machines' keys (vm/vmkeysrv.c)
# against a stand-in for lord-vault's keyring; under ASan and UBSan. The engine's side runs under real QEMU in
# tests/vm_storage_test.sh (run_suites.sh), the app's pure logic in tests/vm_storage_model_test.mjs.
test: build/vmkeysrv-test
build/vmkeysrv-test: tests/vmkeysrv_test.c src/vm/vmkeysrv.c src/vm/vmkeysrv.h src/vm/machine.c src/vm/machine.h src/sec/json.c src/sec/json.h src/doc/wire.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmkeysrv_test.c src/vm/vmkeysrv.c src/vm/machine.c src/sec/json.c src/doc/wire.c -lpthread
	./$@ || { rm -f $@; exit 1; }
