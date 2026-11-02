# LordBench (0.8.6, src/bench, docs/BENCHMARK.md "0.8.6 — LordBench"): lord-bench, the sandboxed helper
# that runs the workloads (CPU, memory, GPU offscreen on a render node fd, storage on a scratch fd,
# sustained load), and lord-server's side (lbrun.c: conditions, the quiet wait, thermal and throttle
# guards, the result file; lord.sysinfo.bench in ui/js_hw.c). The helper links libc, QuickJS (one of
# its workloads), the UI engine and the shaper (ENGINE_SRC: the layout, paint and shaping workloads,
# bench/lbui.c, with their fonts built in) and, with GPU=1, Mesa (GBM, EGL, GLESv2); nothing else.
LB_SRV_SRC := src/bench/lbrun.c src/bench/lbcond.c src/bench/lbproto.c src/bench/bench.c src/bench/lbframes.c src/bench/lbvideo.c src/bench/lbinput.c
LB_SRV_OBJ := $(LB_SRV_SRC:src/%.c=build/obj/%.o)
$(LB_SRV_OBJ): $(wildcard src/bench/*.h) src/hw/rpifw.h src/media/proto.h build/gen/lord_version.h
build/obj/main.o: src/bench/lbframes.h src/bench/lbinput.h
build/obj/ui/grab.o: src/bench/lbinput.h
build/lord-server: $(LB_SRV_OBJ)
build/obj/ui/js_hw.o: src/bench/lbrun.h

# the xz workload's input: a fixed text (src/bench/lbcorpus.c), compressed at build time
build/lbcorpus: src/bench/lbcorpus.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ $<
build/gen/lb_corpus.h: build/lbcorpus
	@mkdir -p build/gen
	./build/lbcorpus text | xz -9 -T1 --check=crc64 -c | ./build/lbcorpus header > $@.new && mv $@.new $@

# the layout, paint and shaping workloads' fonts (bench/lbui.c), built into lord-bench: it opens no files.
# LordOS's Inter, and the Noto fonts subset to the paragraphs of src/bench/lbtext.h (tools/gen-lbfonts.py)
LB_FONTS := ../assets/fonts/Inter-Regular.ttf src/bench/fonts/NotoSansArabic-LB.ttf src/bench/fonts/NotoSansDevanagari-LB.ttf \
            src/bench/fonts/NotoSansThai-LB.ttf src/bench/fonts/NotoSansCJKsc-LB.otf
# Inter is pinned by its SHA-256: the workloads time the shaper and paint with these exact bytes, and the
# references (apps/sysinfo/references.js) were measured with them. A new Inter fails the build here until
# LB_INTER_SHA256 is updated and the references' interface numbers are measured again (docs/BENCHMARK.md).
LB_INTER_SHA256 := 40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82
build/gen/lb_fonts.h: build/lbcorpus $(LB_FONTS)
	@mkdir -p build/gen
	@h=$$( (sha256sum 2>/dev/null || shasum -a 256) < ../assets/fonts/Inter-Regular.ttf | cut -c1-64); \
	  [ "$$h" = "$(LB_INTER_SHA256)" ] || { echo "lord-bench: assets/fonts/Inter-Regular.ttf changed ($$h): update LB_INTER_SHA256 in mk/lordbench.mk and measure the references' cpu.ui/cpu.shape again" >&2; exit 1; }
	{ echo '/* generated from the fonts in mk/lordbench.mk (LB_FONTS) by src/bench/lbcorpus.c */' && \
	  ./build/lbcorpus embed lb_font_inter < ../assets/fonts/Inter-Regular.ttf && \
	  ./build/lbcorpus embed lb_font_arabic < src/bench/fonts/NotoSansArabic-LB.ttf && \
	  ./build/lbcorpus embed lb_font_devanagari < src/bench/fonts/NotoSansDevanagari-LB.ttf && \
	  ./build/lbcorpus embed lb_font_thai < src/bench/fonts/NotoSansThai-LB.ttf && \
	  ./build/lbcorpus embed lb_font_cjk < src/bench/fonts/NotoSansCJKsc-LB.otf; } > $@.new && mv $@.new $@

LB_SRC := src/bench/lbmain.c src/bench/lbwork.c src/bench/lbui.c src/bench/lbproto.c src/bench/bench.c src/pkg/crypto.c src/pkg/xz.c \
          src/sec/sandbox.c src/sec/cgroup.c $(ENGINE_SRC)
LB_GL_SRC := $(if $(filter 1,$(GPU)),src/bench/lbgpu.c src/bench/glscene.c)
LB_GL_FLAGS := $(if $(filter 1,$(GPU)),-DLORD_GPU $(shell pkg-config --cflags gbm egl glesv2))
LB_GL_LIBS := $(if $(filter 1,$(GPU)),$(shell pkg-config --libs gbm egl glesv2))
all: build/lord-bench
build/lord-bench: $(LB_SRC) $(LB_GL_SRC) build/obj/ui/stb.o $(QJS_OBJ) build/gen/lb_corpus.h build/gen/lb_fonts.h $(wildcard src/bench/*.h) src/sec/sandbox.h src/sec/cgroup.h $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LB_GL_FLAGS) $(WARN) -std=c11 -o $@ $(LB_SRC) $(LB_GL_SRC) build/obj/ui/stb.o $(QJS_OBJ) $(LB_GL_LIBS) -lm -lpthread

# the workloads (short; the UI scene and every paragraph's glyphs too), the record checks (the sandbox
# self-test's included), the thermal guard on fake sysfs trees and the boot history
test: build/lordbench-test
build/lordbench-test: tests/lordbench_test.c src/bench/lbwork.c src/bench/lbui.c src/bench/lbproto.c src/bench/lbcond.c src/bench/lbrun.c src/bench/bench.c \
                      src/bench/lbframes.c src/bench/lbinput.c src/bench/lbvideo.c src/bench/boottest.c src/media/proto.c src/pkg/crypto.c src/pkg/xz.c src/hw/rpifw.c src/hw/jbuf.c src/sec/json.c build/obj/ui/stb.o $(QJS_OBJ) build/gen/lb_corpus.h \
                      build/gen/lb_fonts.h build/gen/lord_version.h $(wildcard src/bench/*.h) src/hw/rpifw.h $(ENGINE_SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/lordbench_test.c src/bench/lbwork.c src/bench/lbui.c src/bench/lbproto.c src/bench/lbcond.c \
	    src/bench/lbrun.c src/bench/bench.c src/bench/lbframes.c src/bench/lbinput.c src/bench/lbvideo.c src/bench/boottest.c src/media/proto.c src/pkg/crypto.c src/pkg/xz.c src/hw/rpifw.c src/hw/jbuf.c src/sec/json.c \
	    $(ENGINE_SRC) build/obj/ui/stb.o $(QJS_OBJ) -lm -lpthread
	./$@ || { rm -f $@; exit 1; }
# from a terminal, without a session: build/lordbench-cli quick|full [sustained_s] (needs build/lord-bench, and build/lord-media
# for the video measurements; desktop mode needs a session: without one its frame results are skipped)
LB_CLI_SRC := tests/lordbench_cli.c src/bench/lbrun.c src/bench/lbcond.c src/bench/lbproto.c src/bench/bench.c src/bench/lbframes.c \
              src/bench/lbvideo.c src/bench/boottest.c src/media/proto.c src/hw/rpifw.c src/hw/jbuf.c src/sec/json.c
build/lordbench-cli: $(LB_CLI_SRC) \
                     build/gen/lord_version.h $(wildcard src/bench/*.h) build/lord-bench
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LB_CLI_SRC) -lm -lpthread
