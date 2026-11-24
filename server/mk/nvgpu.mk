# Monitor → GPU's NVIDIA numbers (docs/MONITOR.md, "NVIDIA"; maintainer's approval 2026-11-24): lord-nvgpu, the only
# program that loads NVIDIA's NVML (libnvidia-ml.so.1, dlopen'd from the fixed system folders), sandboxed, numbers only
# over a socketpair. lord-server's side is src/monitor/nvgpu.c (in MON_SRC). lord-nvgpu links only libc (and libdl).
NVGPU_HDR := src/monitor/nvproto.h src/monitor/nvmlmin.h src/monitor/nvgpu.h src/sec/sandbox.h src/sec/cgroup.h
LORD_NVGPU_SRC := src/monitor/nvgpu_main.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
build/lord-nvgpu: $(LORD_NVGPU_SRC) $(NVGPU_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LORD_NVGPU_SRC) -ldl -lpthread
	chmod g-w $@
all: build/lord-nvgpu
build/obj/monitor/nvgpu.o build/obj/monitor/mon.o: src/monitor/nvgpu.h src/monitor/nvproto.h

# Tests only, never installed: the helper with --lib/--probe (-DNVGPU_TEST), a fake NVML, and the test itself (ASan,
# UBSan; parser fuzz, the helper end to end with good and hostile fakes, the sandbox probe).
build/lord-nvgpu-test: $(LORD_NVGPU_SRC) $(NVGPU_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -DNVGPU_TEST -o $@ $(LORD_NVGPU_SRC) -ldl -lpthread
build/libnvfake.so: tests/nvgpu_fake_nvml.c src/monitor/nvmlmin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -shared -fPIC -o $@ tests/nvgpu_fake_nvml.c
test: build/nvgpu-test
build/nvgpu-test: tests/nvgpu_test.c src/monitor/nvgpu.c $(NVGPU_HDR) src/sec/json.c build/lord-nvgpu-test build/libnvfake.so
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -DNVGPU_TEST -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ tests/nvgpu_test.c src/monitor/nvgpu.c src/sec/json.c -lpthread
	./$@ build/lord-nvgpu-test build/libnvfake.so || (rm -f $@; exit 1)
