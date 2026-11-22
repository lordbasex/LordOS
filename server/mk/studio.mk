# LordOS Studio (docs/CAMERA-STUDIO-PLAN.md; phases 0 and 1): our own inference (src/media/studio/nn.c) for MediaPipe's
# Selfie Segmenter and BlazeFace, and lord-studio, lord-media's camera built with LM_STUDIO: capture, segmentation,
# guided filter, blur and picture backgrounds, mirror, on the GPU (GBM + surfaceless EGL + GLES 2, as lord-bench) or
# on the CPU. It links what lord-media links plus Mesa's GBM, EGL and GLES when GPU=1, and nothing else (AGENTS.md,
# E-S1). Without FFmpeg's -dev packages it isn't built, like lord-media.
STUDIO_NN_SRC := src/media/studio/nn.c src/media/studio/face.c
STUDIO_FX_SRC := src/media/studio/stfx.c src/media/studio/stgf.c src/media/studio/stout.c src/media/studio/stlook.c src/media/studio/stframe.c
STUDIO_GL_SRC := $(if $(filter 1,$(GPU)),src/media/studio/stgl.c)
STUDIO_GL_FLAGS := $(if $(filter 1,$(GPU)),-DLORD_GPU $(shell pkg-config --cflags gbm egl glesv2))
STUDIO_GL_LIBS := $(if $(filter 1,$(GPU)),$(shell pkg-config --libs gbm egl glesv2))
STUDIO_HDR := $(wildcard src/media/studio/*.h)
# the inference and the per-pixel effects are built with -O3: GCC's -O2 leaves their channel loops scalar, about three
# times slower (measured: the segmenter takes 11 ms instead of 3.7 ms on an i9-10900)
STUDIO_OBJ := $(patsubst src/media/studio/%.c,build/obj/studio/%.o,$(STUDIO_NN_SRC) $(STUDIO_FX_SRC))
build/obj/studio/%.o: src/media/studio/%.c $(STUDIO_HDR)
	@mkdir -p build/obj/studio
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(WARN) -std=c11 -c -o $@ $<
STUDIO_NN_OBJ := build/obj/studio/nn.o build/obj/studio/face.o

ifeq ($(MEDIA),1)
ALL += build/lord-studio
all: build/lord-studio
endif
LST_SRC := $(LM_SRC) $(STUDIO_GL_SRC) src/media/studio/stcam.c
build/lord-studio: $(LST_SRC) $(STUDIO_OBJ) $(STUDIO_HDR) $(wildcard src/media/helper/*.h) src/media/proto.h src/media/aring.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(MEDIA_CPPFLAGS) -DLM_STUDIO $(STUDIO_GL_FLAGS) $(shell pkg-config --cflags $(MEDIA_PKGS)) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LST_SRC) $(STUDIO_OBJ) \
	    $(shell pkg-config --libs $(MEDIA_PKGS)) $(STUDIO_GL_LIBS) -lpthread -lm

# the inference against ONNX Runtime's stored outputs, face decoding and the loader's fuzz (tests/run_suites.sh runs it)
test: build/studio-nn-test
build/studio-nn-test: tests/studio_nn_test.c $(STUDIO_NN_OBJ) $(STUDIO_HDR) src/pkg/crypto.c build/obj/ui/stb.o
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/studio_nn_test.c $(STUDIO_NN_OBJ) src/pkg/crypto.c build/obj/ui/stb.o -lm

# the pipeline on fixed frames: matte, guided filter, blur and picture composites, mirror; CPU always, GPU when there is
# a render node (the two must agree)
test: build/studio-test
build/studio-test: tests/studio_test.c $(STUDIO_OBJ) $(STUDIO_GL_SRC) $(STUDIO_HDR) src/pkg/crypto.c build/obj/ui/stb.o
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(STUDIO_GL_FLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/studio_test.c $(STUDIO_OBJ) $(STUDIO_GL_SRC) \
	    src/pkg/crypto.c build/obj/ui/stb.o $(STUDIO_GL_LIBS) -lm -lpthread

# phase 3's looks on fixed frames: Studio Light, colour match, filters, the green screen, a video background, Center
# Stage and Retouch; CPU always, GPU when there is a render node (tests/run_suites.sh runs it)
test: build/studio-fx-test
build/studio-fx-test: tests/studio_fx_test.c $(STUDIO_OBJ) $(STUDIO_GL_SRC) $(STUDIO_HDR) build/obj/ui/stb.o
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(STUDIO_GL_FLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/studio_fx_test.c $(STUDIO_OBJ) $(STUDIO_GL_SRC) \
	    build/obj/ui/stb.o $(STUDIO_GL_LIBS) -lm -lpthread

# phase 3, the background catalog: the closed actions studio-bg-get and studio-bg-remove (E-S4) with a fake download,
# their catalog lookup, and the shipped assets/studio/backgrounds.json (licenses, entries the action accepts, thumbnails)
test: build/studio-bg-admin-test
build/studio-bg-admin-test: tests/studio_bg_admin_test.c src/hw/studio_bg_admin.c src/hw/studio_bg_find.c src/hw/studio_bg_find.h src/sec/json.c src/pkg/crypto.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -D'STUDIO_BG_CATALOG="build/studio-bg-test/catalog.json"' \
	    -D'STUDIO_BG_STORE="build/studio-bg-test/store"' -D'STUDIO_BG_OWNER_OK(st)=((st).st_uid == getuid())' \
	    -o $@ tests/studio_bg_admin_test.c src/hw/studio_bg_admin.c src/hw/studio_bg_find.c src/sec/json.c src/pkg/crypto.c

# lord-studio itself without a camera: the pipeline inside its sandbox, refusals (tests/run_suites.sh full)
test-full: build/studio-helper-test
build/studio-helper-test: tests/studio_helper_test.c src/media/proto.c src/media/proto.h src/media/studio/models.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/studio_helper_test.c src/media/proto.c

# phase 2, LordOS Studio's camera for every app: what the loopback shows, its readers named for the indicator, the idle
# and off cards and the YUYV conversion (no v4l2loopback needed; tests/run_suites.sh runs it)
test: build/studio-hub-test
build/studio-hub-test: tests/studio_hub_test.c src/media/mstudiohub.c src/media/mstudiohub.h src/media/studio/stout.c src/media/studio/stout.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/studio_hub_test.c src/media/mstudiohub.c src/media/studio/stout.c -lm
