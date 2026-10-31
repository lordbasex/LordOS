# Cleaner (docs/CLEANER-ARCHITECTURE.md): lord-clean, the sandboxed read-only scanner, and lord-server's
# side (src/clean: job thread, rule table, plans, execution; lord.clean in ui/js_clean.c). The helper
# links libc and the sandbox, the channel and the JSON reader lord-doc uses, fsops.c for fsops_protected,
# so the list of protected paths has one home, and the stb_image the project already carries, for similar
# images: the only decoding in Cleaner, built as lord-maild's (helper/lcimage.c).
CLEAN_SRV_SRC := src/clean/clean.c src/clean/cleanfs.c src/clean/rules.c src/clean/clean_admin.c src/ui/js_clean.c
CLEAN_SRV_OBJ := $(CLEAN_SRV_SRC:src/%.c=build/obj/%.o)
$(CLEAN_SRV_OBJ): $(wildcard src/clean/*.h) src/fsops.h src/doc/wire.h src/doc/proto.h src/sec/json.h src/ui/js_clean.h src/ui/js_err.h src/errcode.h
build/lord-server: $(CLEAN_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_clean.h
build/obj/admin.o: src/clean/clean_admin.h

CLEAN_HELPER_SRC := src/clean/helper/lcmain.c src/clean/cleanfs.c src/doc/wire.c src/fsops.c src/pkg/crypto.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
all: build/lord-clean
build/obj/clean/helper/lcimage.o: src/clean/helper/lcimage.c src/clean/helper/lcimage.h src/clean/cleanproto.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -w -c -o $@ $<
build/lord-clean: $(CLEAN_HELPER_SRC) build/obj/clean/helper/lcimage.o src/clean/helper/lcimage.h $(wildcard src/clean/*.h) src/fsops.h src/doc/wire.h src/doc/proto.h src/sec/json.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(CLEAN_HELPER_SRC) build/obj/clean/helper/lcimage.o -lpthread -lm
	chmod g-w $@

# in throwaway folders only: the rule table, the helper's walk (links, mounts, limits, protected paths),
# what lord-server accepts from it, plans, execution, cancel. Needs build/lord-clean next to it.
test: build/clean-test
CLEAN_TEST_SRC := tests/clean_test.c src/clean/clean.c src/clean/cleanfs.c src/clean/rules.c src/clean/clean_admin.c src/doc/wire.c src/fsops.c src/sec/json.c
build/clean-test: $(CLEAN_TEST_SRC) $(wildcard src/clean/*.h) src/fsops.h build/lord-clean
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DCLEAN_TEST $(CFLAGS) $(WARN) -std=c11 -o $@ $(CLEAN_TEST_SRC) tests/clean_admin_stub.c -lm -lpthread
	./$@ || { rm -f $@; exit 1; }

# times, memory and call costs on a throwaway folder: build/clean-bench <folder> (docs/CLEANER-TESTS.md)
build/clean-bench: tests/clean_bench.c tests/clean_admin_stub.c src/clean/clean.c src/clean/cleanfs.c src/clean/rules.c src/clean/clean_admin.c src/doc/wire.c src/fsops.c src/sec/json.c build/lord-clean
	$(CC) $(CPPFLAGS) -DCLEAN_TEST $(CFLAGS) $(WARN) -std=c11 -o $@ tests/clean_bench.c tests/clean_admin_stub.c src/clean/clean.c src/clean/cleanfs.c src/clean/rules.c src/clean/clean_admin.c src/doc/wire.c src/fsops.c src/sec/json.c -lm -lpthread
