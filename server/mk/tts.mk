# Voice (docs/VOICE.md): lord-tts, the sandboxed text-to-speech helper (src/doc/h_tts.c on lord-doc's helper loop),
# and lord-say, the `say` command (src/tts/say.c). lord-tts is the only binary that links audio.cpp (Apache-2.0) and
# its ggml: audio.cpp at the commit tools/tts/fetch-audiocpp.sh pins, in third_party/audiocpp, built with the Pocket
# TTS family only, the CPU kernels of every x86-64 generation (picked when it starts) and Vulkan. The libraries go in
# build/lib/tts (installed in /usr/lib/lordos/lib/tts), where audio.cpp loads its ggml backends from. Without the
# source, cmake, Vulkan's headers or glslc, make skips lord-tts (TTSHELPER=0); lord-say needs nothing.
AUDIOCPP_DIR := ../third_party/audiocpp
TTSHELPER ?= $(shell [ -f $(AUDIOCPP_DIR)/include/audiocpp.h ] && command -v cmake >/dev/null 2>&1 && command -v glslc >/dev/null 2>&1 && pkg-config --exists vulkan 2>/dev/null && echo 1 || echo 0)
# `all:` here, not ALL +=: the Makefile's `all: $(ALL)` is read before mk/*.mk are included, so what is added to
# ALL here never reached it (as mk/lan.mk does)
ALL += build/lord-say
all: build/lord-say
ifeq ($(TTSHELPER),1)
ALL += build/lord-tts
all: build/lord-tts
endif

AUDIOCPP_FLAGS := -DCMAKE_BUILD_TYPE=Release -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=pocket_tts -DAUDIOCPP_BUILD_C_API=ON \
                  -DENGINE_ENABLE_VULKAN=ON -DENGINE_ENABLE_CUDA=OFF -DENGINE_ENABLE_CPU_ALL_VARIANTS=ON \
                  -DAUDIOCPP_BUILD_NATIVE_MODEL_MANAGER=OFF
build/lib/tts/libaudiocpp.so.0: $(AUDIOCPP_DIR)/include/audiocpp.h
	@mkdir -p build/audiocpp build/lib/tts
	cmake -S $(AUDIOCPP_DIR) -B build/audiocpp $(AUDIOCPP_FLAGS) > build/audiocpp.log 2>&1 || { tail -20 build/audiocpp.log; exit 1; }
	cmake --build build/audiocpp --parallel $(shell nproc) --target audiocpp >> build/audiocpp.log 2>&1 || { tail -30 build/audiocpp.log; exit 1; }
	rm -f build/lib/tts/*.so*
	cp -P build/audiocpp/bin/libaudiocpp.so* build/audiocpp/bin/libggml*.so* build/lib/tts/

# the HD models are Kyutai's own folders (model.safetensors, tokenizer.model, embeddings/): audio.cpp's description of
# Pocket TTS, made to read that folder itself rather than languages/english under it (h_tts.c)
ifeq ($(TTSHELPER),1)
ALL += build/lib/tts/pocket_tts_dir.json
all: build/lib/tts/pocket_tts_dir.json
endif
build/lib/tts/pocket_tts_dir.json: $(AUDIOCPP_DIR)/model_specs/pocket_tts.json
	@mkdir -p build/lib/tts
	python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); [s.update(roots={"language": "."}) for s in d["sources"] if s["format"] == "safetensors"]; json.dump(d, open(sys.argv[2], "w"), indent=1)' $< $@

LT_SRC := src/doc/helper.c src/doc/h_tts.c src/tts/hiss.c src/doc/wire.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
build/lord-tts: $(LT_SRC) build/lib/tts/libaudiocpp.so.0 $(wildcard src/doc/*.h) src/tts/hiss.h src/sec/sandbox.h src/sec/cgroup.h src/sec/json.h
	$(CC) $(CPPFLAGS) -DLORD_HELPER_TTS $(CFLAGS) $(WARN) -std=c11 -isystem $(AUDIOCPP_DIR)/include -o $@ $(LT_SRC) \
		-Lbuild/lib/tts -l:libaudiocpp.so.0 -Wl,-rpath,'$$ORIGIN/../lib/tts:$$ORIGIN/lib/tts' -lm

build/lord-say: src/tts/say.c src/tts/saytext.c src/tts/saytext.h src/tts/levels.c src/tts/levels.h src/doc/wire.c src/sec/json.c src/doc/wire.h src/doc/proto.h src/sec/json.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ src/tts/say.c src/tts/saytext.c src/tts/levels.c src/doc/wire.c src/sec/json.c -lm

# how say splits text into sentences (tests/saytext_test.c), the bands of the player's wave (tests/levels_test.c) and
# lord-tts's hiss measure (tests/hiss_test.c); lord-tts's own test needs a model (tests/tts_test.sh)
test: build/saytext-test build/levels-test build/hiss-test
build/levels-test: tests/levels_test.c src/tts/levels.c src/tts/levels.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/levels_test.c src/tts/levels.c -lm
	./$@ || { rm -f $@; exit 1; }
build/hiss-test: tests/hiss_test.c src/tts/hiss.c src/tts/hiss.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/hiss_test.c src/tts/hiss.c -lm
	./$@ || { rm -f $@; exit 1; }
build/saytext-test: tests/saytext_test.c src/tts/saytext.c src/tts/saytext.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/saytext_test.c src/tts/saytext.c
	./$@ || { rm -f $@; exit 1; }
