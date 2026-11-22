# flatpak-view-probe (tests/flatpak_view_probe.c): does bubblewrap start inside the launcher's view? Run by hand on a
# test machine (docs/FLATPAK.md, Problem 1). Not part of `make test`.
build/flatpak-view-probe: tests/flatpak_view_probe.c src/sec/sandbox.c src/sec/cgroup.c src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/flatpak_view_probe.c src/sec/sandbox.c src/sec/cgroup.c -lpthread

# Flatpak apps for the launcher: ids, the installation, the `flatpak run` command line (src/fpak.c)
test: build/fpak-test
build/fpak-test: tests/fpak_test.c src/fpak.c src/fpak.h src/fw/rules.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/fpak_test.c src/fpak.c
	./$@ || (rm -f $@; exit 1)

# A Flatpak app's Spawn helper (src/fpspawner.c): its checks, and the helper over a socket pair as lord-server drives it,
# with this test standing in for /usr/bin/flatpak (fds placed, instance fd, process group, exit status, refusals)
test: build/fpspawner-test
build/fpspawner-test: tests/fpspawner_test.c src/fpspawner.c src/fpspawner.h src/fpak.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE '-DFPSW_EXEC="/proc/self/exe"' -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ tests/fpspawner_test.c src/fpspawner.c
	./$@ || (rm -f $@; exit 1)

# tests/fpak_cli.c: one Flathub management command through the real runner (src/fpakrun.c), for the VM; never installed
build/fpak-cli: tests/fpak_cli.c src/fpakrun.c src/fpakrun.h src/fpak.c src/fpak.h src/sec/cgroup.c src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/fpak_cli.c src/fpakrun.c src/fpak.c src/sec/cgroup.c

# Flathub's AppStream to the catalog index (src/fpakas.c, run only in lord-appstream), and its lines checked again
test: build/fpakas-test
build/fpakas-test: tests/fpakas_test.c src/fpakas.c src/fpakas.h src/fpak.c src/fpak.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ tests/fpakas_test.c src/fpakas.c src/fpak.c
	./$@ || (rm -f $@; exit 1)

# lord-appstream (src/fpakasmain.c, AGENTS.md E7): the sandboxed reader of Flathub's catalog; only the C library
LORD_APPSTREAM_SRC := src/fpakasmain.c src/fpakas.c src/fpak.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
build/lord-appstream: $(LORD_APPSTREAM_SRC) src/fpakas.h src/fpak.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ $(LORD_APPSTREAM_SRC) -lpthread
all: build/lord-appstream

# the launcher's view of a home with mounts under it (tests/sandbox_home_mounts_test.c; 77: no user namespaces here)
test: build/sandbox-home-mounts-test
build/sandbox-home-mounts-test: tests/sandbox_home_mounts_test.c src/sec/sandbox.c src/sec/cgroup.c src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/sandbox_home_mounts_test.c src/sec/sandbox.c src/sec/cgroup.c -lpthread
	./$@; r=$$?; [ $$r = 0 ] || [ $$r = 77 ] || { rm -f $@; exit 1; }

# how long the launcher's view lives: what an app leaves behind keeps running, its Spawn helper is ended after it
# (tests/sandbox_view_lifetime_test.c; 77: no user namespaces here)
test: build/sandbox-view-lifetime-test
build/sandbox-view-lifetime-test: tests/sandbox_view_lifetime_test.c src/sec/sandbox.c src/sec/cgroup.c src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/sandbox_view_lifetime_test.c src/sec/sandbox.c src/sec/cgroup.c -lpthread
	./$@; r=$$?; [ $$r = 0 ] || [ $$r = 77 ] || { rm -f $@; exit 1; }

# a Flatpak app's shared runtime folders and the chosen file lent into its running view, with a fake app (and inside
# bubblewrap when it is installed) (tests/sandbox_lend_test.c; 77: no user namespaces here)
test: build/sandbox-lend-test
build/sandbox-lend-test: tests/sandbox_lend_test.c src/sec/sandbox.c src/sec/cgroup.c src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/sandbox_lend_test.c src/sec/sandbox.c src/sec/cgroup.c -lpthread
	./$@; r=$$?; [ $$r = 0 ] || [ $$r = 77 ] || { rm -f $@; exit 1; }

# tests/fpportal_probe.c: a fake Flatpak app's side of the portals, run inside a real sandbox for the end-to-end checks
# (docs/FLATPAK.md, "Lending a chosen file"); static, so it runs on any runtime. A test tool: never installed.
build/fpportal-probe: tests/fpportal_probe.c src/vault/dbus.c src/vault/dbus.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -static -o $@ tests/fpportal_probe.c src/vault/dbus.c
