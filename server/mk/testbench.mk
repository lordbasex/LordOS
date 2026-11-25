# lordos-testbench (src/testbench, docs/PAPER-VALIDATION.md): the visual test bench, a plain Wayland
# client (test card, frame and presentation timing, every key, the pointer, the compositor's globals).
# libwayland-client and libxkbcommon only; it reads no files, except --toolkits (src/testbench/tkrun.c,
# docs/TOOLKIT-AUDIT.md), which runs the Python harnesses in /usr/lib/lordos/toolkit-audit and reads their output. The measurement pieces (src/bench: frame
# times, presentation, latency, the GL renderer's kind, the 3D scene, the report file) are shared with
# LordBench (0.8.6, docs/BENCHMARK.md) and tested on their own (build/bench-test).
BENCH_SRC := src/bench/bench.c
TESTBENCH_SRC := src/testbench/testbench.c src/testbench/checks.c src/testbench/globtable.c src/testbench/gl3d.c src/testbench/card.c src/testbench/draw.c src/testbench/toolkits.c src/testbench/tkrun.c src/sec/json.c $(BENCH_SRC)
all: build/lordos-testbench
# the 3D view needs EGL, GLESv2 and wayland-egl (Mesa); without them the bench builds and says so
TB_GL := $(shell pkg-config --exists egl glesv2 wayland-egl 2>/dev/null && echo 1)
TB_GL_FLAGS := $(if $(TB_GL),-DTB_GL $(shell pkg-config --cflags egl glesv2 wayland-egl))
TESTBENCH_SRC += $(if $(TB_GL),src/bench/glscene.c)
TB_GL_LIBS := $(if $(TB_GL),$(shell pkg-config --libs egl glesv2 wayland-egl))
build/lordos-testbench: $(TESTBENCH_SRC) $(wildcard src/testbench/*.h src/bench/*.h) $(CLIENT_GEN)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TB_GL_FLAGS) -Ibuild/gen $(WARN) -std=c11 -o $@ $(TESTBENCH_SRC) $(filter %.c,$(CLIENT_GEN)) -lwayland-client -lxkbcommon -lm $(TB_GL_LIBS)
test: build/bench-test
build/bench-test: tests/bench_test.c $(BENCH_SRC) src/bench/bench.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/bench_test.c $(BENCH_SRC) -lm
	./$@ || { rm -f $@; exit 1; }
# the registry against LordOS's protocol table (globtable.c, expected_globals.h generated from docs/wayland-protocols.json):
# offered, newer, older, missing, offered only to some clients; hostile interface names in the JSON
test: build/testbench-globals-test build/lordos-testbench
build/testbench-globals-test: tests/testbench_globals_test.c src/testbench/globtable.c src/testbench/globtable.h src/testbench/expected_globals.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ tests/testbench_globals_test.c src/testbench/globtable.c
	./$@ || { rm -f $@; exit 1; }
# the toolkit audit (--toolkits): the scoring, the WAYLAND_DEBUG and stderr reader, the harness's line (hostile
# output: oversized lines, bad JSON, control characters), and real runs of fake harnesses (fixed JSON, a crash, a
# hang killed at the deadline) through a fake python3 (tests/toolkit_audit/)
TK_SRC := src/testbench/toolkits.c src/testbench/tkrun.c src/testbench/globtable.c src/sec/json.c
test: build/toolkit-audit-test
build/toolkit-audit-test: tests/toolkit_audit_test.c $(TK_SRC) src/testbench/toolkits.h src/testbench/tkrun.h src/testbench/expected_globals.h $(wildcard tests/toolkit_audit/*)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ tests/toolkit_audit_test.c $(TK_SRC) -lm
	./$@ || { rm -f $@; exit 1; }
