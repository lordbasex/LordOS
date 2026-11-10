# Sharing a screen or a window with Linux apps (0.8.8.8, docs/WAYLAND.md "Sharing a screen or a window"): our own
# org.freedesktop.portal.ScreenCast in lord-server (ui/portal.c, its sessions in cast/castsm.c) and lord-screencast,
# the sandboxed helper that makes the PipeWire video source (cast/lordscreencast.c). lord-screencast is the only
# binary that links libpipewire-0.3; lord-server only connects the socket and hands it over. Without
# libpipewire-0.3-dev everything else builds and the portal says it has no source types.
CAST_HDR := src/cast/castproto.h src/cast/castsm.h src/cast/cast.h

# lord-server's side: the sessions, the frames (cast/cast.c) and the protocol
CASTS_OBJ := build/obj/cast/cast.o build/obj/cast/castproto.o build/obj/cast/castsm.o
build/lord-server: $(CASTS_OBJ)
$(CASTS_OBJ): $(CAST_HDR) src/ui/ui.h src/media/media.h src/media/media_int.h src/media/imgops.h
build/obj/ui/portal.o build/obj/ui/js_hw.o build/obj/main.o build/obj/remote/mirror.o: $(CAST_HDR)

SCREENCAST ?= $(shell pkg-config --exists libpipewire-0.3 2>/dev/null && echo 1 || echo 0)
LSC_SRC := src/cast/lordscreencast.c src/cast/castproto.c src/sec/sandbox.c src/sec/cgroup.c
ifeq ($(SCREENCAST),1)
all: build/lord-screencast
endif
build/lord-screencast: $(LSC_SRC) src/cast/castproto.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 $(shell pkg-config --cflags libpipewire-0.3) -o $@ $(LSC_SRC) \
	    $(shell pkg-config --libs libpipewire-0.3) -lpthread

# the portal's sessions (tests/castsm_test.c) and the ring and channel (tests/castproto_test.c), no bus or helper
test: build/castsm-test build/castproto-test
build/castsm-test: tests/castsm_test.c src/cast/castsm.c src/cast/castsm.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/castsm_test.c src/cast/castsm.c
	./$@ || { rm -f $@; exit 1; }
build/castproto-test: tests/castproto_test.c src/cast/castproto.c src/cast/castproto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ tests/castproto_test.c src/cast/castproto.c
	./$@ || { rm -f $@; exit 1; }

# lord-screencast against a private PipeWire daemon (tests/screencast_test.c, run by tests/screencast_test.sh): the
# node, frames that reach a consumer pixel for pixel, a resize, the restricted connection that sees only the stream,
# and the cost of 1920x1080 at 30 frames a second. "skipped: needs pipewire" without the daemon.
ifeq ($(SCREENCAST),1)
test: build/screencast-test
endif
build/screencast-test: tests/screencast_test.c src/cast/castproto.c src/cast/castproto.h build/lord-screencast
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 $(shell pkg-config --cflags libpipewire-0.3) -o $@ tests/screencast_test.c src/cast/castproto.c \
	    $(shell pkg-config --libs libpipewire-0.3) -lpthread
	sh tests/screencast_test.sh build || { rm -f $@; exit 1; }
# what sharing 1920x1080 at 30 frames a second costs (docs/WAYLAND.md has the numbers): make -C server screencast-bench
.PHONY: screencast-bench
screencast-bench: build/screencast-test
	sh tests/screencast_test.sh build bench
