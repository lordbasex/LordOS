# Web apps (0.8.8.8 in development, docs/WEBAPPS.md): lord-web, the LordOS Web Runtime, one process per installed web
# app and profile, and the only binary that links WPE WebKit (Debian 13's libwpewebkit-2.0 2.48, libwpe 1.16 and
# wpebackend-fdo 1.16, with GLib/GIO, GStreamer and libsoup through them). It is a plain Wayland client
# (libwayland-client, xkbcommon, EGL for the dmabuf path; libwayland-server only for wpebackend-fdo's own buffers)
# and a D-Bus client of the session (GIO). It ships in the optional package lordos-web (tools/iso/mkdeb-web.sh).
# Built only when pkg-config finds WPE WebKit and its friends; WEB=0 or a missing package skips it with a message and
# everything else builds as before. lord-server never links or loads WebKit.
WEB_PKGS := wpe-webkit-2.0 wpe-1.0 wpebackend-fdo-1.0 egl wayland-client wayland-server xkbcommon gio-unix-2.0
WEB ?= $(shell pkg-config --exists $(WEB_PKGS) 2>/dev/null && echo 1 || echo 0)

# the pure checks: argv, the registry, addresses and scope, the probe's answer, the icon check, download names, the wheel,
# the unread count, the user agent, the notification sound, dropped files, the Edit commands and context menu items
WEB_PURE_SRC := src/web/webargs.c src/web/webreg.c src/web/weburl.c src/web/webmanifest.c src/web/webdl.c src/web/webaxis.c src/web/webpage.c \
                src/web/webdrop.c src/web/webedit.c src/sec/json.c
WEB_PURE_HDR := src/web/webargs.h src/web/webreg.h src/web/weburl.h src/web/webmanifest.h src/web/webdl.h src/web/webaxis.h src/web/webpage.h \
                src/web/webdrop.h src/web/webedit.h src/sec/json.h

test: build/webapp-test
build/webapp-test: tests/webapp_test.c $(WEB_PURE_SRC) $(WEB_PURE_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/webapp_test.c $(WEB_PURE_SRC)
	./$@ || { rm -f $@; exit 1; }

ifeq ($(WEB),1)
ALL += build/lord-web
all: build/lord-web
# the smoke test (tests/web_probe_test.py from tests/run_suites.sh) probes a page served by the test itself
test: build/lord-web
# webreg-test checks that lord-server's lookup finds this lord-web next to it (webmgr_available)
build/webreg-test: | build/lord-web

# xdg-activation is the one protocol of lord-web's that the test clients' CLIENT_PROTO (Makefile) doesn't have
WEB_PROTO := staging/xdg-activation/xdg-activation-v1
$(foreach p,$(WEB_PROTO),$(eval $(call wl_client_proto,$(p))))
WEB_GEN := $(foreach p,$(WEB_PROTO),build/gen/client/$(notdir $(p))-client-protocol.h build/gen/client/$(notdir $(p))-protocol.c)
WEB_CLIENT_C := $(filter %/xdg-shell-protocol.c %/xdg-decoration-unstable-v1-protocol.c %/linux-dmabuf-v1-protocol.c %/viewporter-protocol.c \
                         %/fractional-scale-v1-protocol.c %/text-input-unstable-v3-protocol.c %/cursor-shape-v1-protocol.c %/tablet-v2-protocol.c,$(CLIENT_GEN)) \
                $(filter %.c,$(WEB_GEN))

WEB_SRC := src/web/webmain.c src/web/webengine.c src/web/webwin.c src/web/webshm.c src/web/webbus.c src/web/webtray.c src/web/webprobe.c \
           src/web/weblang.c src/web/webxfer.c src/web/webselect.c $(WEB_PURE_SRC) src/sec/cgroup.c src/sec/sandbox.c
WEB_HDR := src/web/web.h src/web/webwin.h $(WEB_PURE_HDR) src/sec/cgroup.h src/sec/sandbox.h

# WebKit's headers are not ours: -isystem, so -Wall -Wextra apply to lord-web's own code only
WEB_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(WEB_PKGS)))
WEB_LIBS := $(shell pkg-config --libs $(WEB_PKGS))

build/lord-web: $(WEB_SRC) $(WEB_HDR) $(CLIENT_GEN) $(WEB_GEN) build/obj/ui/stb.o build/gen/lord_version.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -Ibuild/gen/client $(WEB_CFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(WEB_SRC) $(WEB_CLIENT_C) build/obj/ui/stb.o $(WEB_LIBS) -lm
else
all: web-skipped
.PHONY: web-skipped
web-skipped:
	@echo "lord-web skipped: needs $(WEB_PKGS) (libwpewebkit-2.0-dev, libwpebackend-fdo-1.0-dev, libegl-dev; docs/VM-SETUP.md) or WEB=0 was given"
endif
