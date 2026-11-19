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
