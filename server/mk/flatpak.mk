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
