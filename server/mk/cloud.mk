# Cloud (docs/CLOUD.md): lord-server's side of the person's cloud accounts. A thread runs rclone rcd (the lordos-rclone
# package, /usr/lib/lordos/rclone/rclone) on a Unix socket, in its own scope, and is its only client (src/cloud, with
# lord.cloud in ui/js_cloud.c). Secrets go to the keyring through lord-vault (cloud_kr.c).
CLOUD_SRV_SRC := src/cloud/cloud.c src/cloud/cloud_files.c src/cloud/cloud_rc.c src/cloud/cloud_pure.c src/cloud/cloud_kr.c src/ui/js_cloud.c
CLOUD_SRV_OBJ := $(CLOUD_SRV_SRC:src/%.c=build/obj/%.o)
CLOUD_HDR := $(wildcard src/cloud/*.h) src/doc/wire.h src/sec/json.h src/sec/cgroup.h src/sec/sandbox.h src/errcode.h src/vault/vclient.h src/apps.h src/ui/js_cloud.h src/ui/js.h
$(CLOUD_SRV_OBJ): $(CLOUD_HDR)
build/lord-server: $(CLOUD_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_cloud.h
build/obj/main.o: src/cloud/cloud.h src/ui/js_cloud.h

# the closed rc method table, argument and path checks, authorize's output, the status merge, the eviction choice,
# and Free Up Space on a real cache tree in a throwaway home (no rclone needed)
test: build/cloud-test
CLOUD_CORE_SRC := src/cloud/cloud.c src/cloud/cloud_files.c src/cloud/cloud_rc.c src/cloud/cloud_pure.c src/doc/wire.c src/sec/json.c src/sec/cgroup.c src/sec/sandbox.c
build/cloud-test: tests/cloud_test.c $(CLOUD_CORE_SRC) $(wildcard src/cloud/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DCLOUD_TEST $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ tests/cloud_test.c $(CLOUD_CORE_SRC) -lpthread -lcrypt -lm
	ASAN_OPTIONS=detect_leaks=0 ./$@ || { rm -f $@; exit 1; }

# the same code on its own, driven by tests/cloud_integration.sh against a local WebDAV server (built by make test,
# run by hand: sh tests/cloud_integration.sh build <rclone>)
test: build/cloud-harness
build/cloud-harness: tests/cloud_harness.c $(CLOUD_CORE_SRC) $(wildcard src/cloud/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DCLOUD_TEST $(CFLAGS) $(WARN) -std=c11 -o $@ tests/cloud_harness.c $(CLOUD_CORE_SRC) -lpthread -lcrypt -lm

# lord.fs's Recents and search walks alone (tests/cloud_integration.sh: they must not enter a cloud mount that hangs)
test: build/fs-walk
build/fs-walk: tests/fs_walk_cli.c src/fsops.c src/fsops.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fs_walk_cli.c src/fsops.c -lpthread
