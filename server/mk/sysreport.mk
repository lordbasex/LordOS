# System Information (0.8.5, src/sysreport, docs/BENCHMARK.md): the collector, shared by the
# `axia-sysreport` and `axia-fetch` commands (one binary; build/axia-fetch is a link) and
# (later) lord.sysinfo. Read-only, no root, libc only; `bench` starts the sandboxed axia-bench next to it.
SYSREPORT_SRC := src/sysreport/sysreport.c src/sysreport/fetch.c src/sysreport/smbios.c src/sysreport/facts.c src/sysreport/sysreport_out.c src/sysreport/ids.c src/hw/edid.c src/hw/rpifw.c src/hw/jbuf.c src/sec/json.c
SYSREPORT_HDR := $(wildcard src/sysreport/*.h) src/hw/edid.h src/hw/rpifw.h src/hw/jbuf.h src/sec/json.h
all: build/axia-sysreport build/axia-fetch
build/axia-fetch: build/axia-sysreport
	ln -sf axia-sysreport $@
# `axia-sysreport bench` links LordBench's axia-server side (mk/lordbench.mk), libc only too
SYSREPORT_BENCH_SRC := src/bench/lbrun.c src/bench/lbcond.c src/bench/lbproto.c src/bench/bench.c src/bench/lbframes.c \
                       src/bench/lbvideo.c src/bench/boottest.c src/media/proto.c
build/axia-sysreport: src/sysreport/cli.c $(SYSREPORT_SRC) $(SYSREPORT_HDR) $(SYSREPORT_BENCH_SRC) $(wildcard src/bench/*.h) build/gen/lord_version.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -Ibuild/gen $(CFLAGS) $(WARN) -std=c11 -o $@ src/sysreport/cli.c $(SYSREPORT_SRC) $(SYSREPORT_BENCH_SRC) -lm -lpthread
# five fake machines full of serials, UUIDs, MACs and names that must not reach a report (periph.c is
# lord.sysinfo.peripherals(): on screen only, so it is not in SYSREPORT_SRC and the commands can't print it);
# `build/sysreport-test fixture <dir> amd|intel|nvidia|rpi5|vm` writes one for `--root`
test: build/sysreport-test
build/sysreport-test: tests/sysreport_test.c src/sysreport/periph.c $(SYSREPORT_SRC) $(SYSREPORT_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/sysreport_test.c src/sysreport/periph.c $(SYSREPORT_SRC)
	./$@ || { rm -f $@; exit 1; }
# (0.8.7) the catalogue's signed snapshot (src/sysreport/hwsnap.c, lord.sysinfo.community()): signature first,
# structure, limits, no links; and the snapshot tools/hwreport/catalog.mjs wrote, signed with a test key
test: build/hwsnap-test
build/hwsnap-test: tests/hwsnap_test.c src/sysreport/hwsnap.c src/sec/codesig.c src/sec/json.c src/pkg/crypto.c src/sysreport/hwsnap.h src/sec/codesig.h src/sec/json.h src/pkg/crypto.h build/gen/codesig_build.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/hwsnap_test.c src/sysreport/hwsnap.c src/sec/codesig.c src/sec/json.c src/pkg/crypto.c -lpthread
	./$@ ../assets/hwreport/snapshot.json || { rm -f $@; exit 1; }
