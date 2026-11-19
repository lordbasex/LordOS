# flatpak-view-probe (tests/flatpak_view_probe.c): does bubblewrap start inside the launcher's view? Run by hand on a
# test machine (docs/FLATPAK.md, Problem 1). Not part of `make test`.
build/flatpak-view-probe: tests/flatpak_view_probe.c src/sec/sandbox.c src/sec/cgroup.c src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D_GNU_SOURCE -o $@ tests/flatpak_view_probe.c src/sec/sandbox.c src/sec/cgroup.c -lpthread
