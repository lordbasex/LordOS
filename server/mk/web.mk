# Web apps (0.8.8.8 in development, docs/WEBAPPS.md): lord-web, the LordOS Web Runtime, one process per installed web
# app and profile, and the only binary that links WPE WebKit (with GLib/GIO, GStreamer and libsoup through it). It is a
# plain Wayland client (libwayland-client, xkbcommon, EGL for the dmabuf path) and a D-Bus client of the session (GIO).
# It ships in the optional package lordos-web (tools/iso/mkdeb-web.sh). lord-server never links or loads WebKit.
#
# Two backends for WebKit's view (src/web/webback.h), one of them built:
#   platform  WPEPlatform (wpe-platform-2.0: WPE WebKit 2.50 and later built with it; LordOS's 2.54 backport)
#   legacy    libwpe + wpebackend-fdo (Debian 13's libwpewebkit-2.0 2.48, libwpe 1.16, wpebackend-fdo 1.16;
#             libwayland-server only for wpebackend-fdo's own buffers)
# platform when pkg-config finds it, else legacy; LORD_WEB_BACKEND=legacy|platform forces one (an error when its
# packages are missing). Without either, or with WEB=0, lord-web is skipped and the build says why; everything else
# builds as before.
WEB_COMMON_PKGS := wpe-webkit-2.0 egl wayland-client xkbcommon gio-unix-2.0
WEB_LEGACY_PKGS := $(WEB_COMMON_PKGS) wpe-1.0 wpebackend-fdo-1.0 wayland-server
WEB_PLATFORM_PKGS := $(WEB_COMMON_PKGS) wpe-platform-2.0
web_missing = $(strip $(foreach p,$(1),$(if $(shell pkg-config --exists $(p) 2>/dev/null && echo y),,$(p))))
LORD_WEB_FORCED := $(if $(filter undefined,$(origin LORD_WEB_BACKEND)),,$(LORD_WEB_BACKEND))
ifeq ($(origin LORD_WEB_BACKEND),undefined)
LORD_WEB_BACKEND := $(if $(call web_missing,$(WEB_PLATFORM_PKGS)),$(if $(call web_missing,$(WEB_LEGACY_PKGS)),none,legacy),platform)
endif
ifeq ($(LORD_WEB_BACKEND),platform)
WEB_PKGS := $(WEB_PLATFORM_PKGS)
else ifeq ($(LORD_WEB_BACKEND),legacy)
WEB_PKGS := $(WEB_LEGACY_PKGS)
else ifeq ($(LORD_WEB_BACKEND),none)
WEB_PKGS := $(WEB_LEGACY_PKGS)
else
$(error LORD_WEB_BACKEND must be legacy or platform, not "$(LORD_WEB_BACKEND)")
endif
WEB_MISSING := $(call web_missing,$(WEB_PKGS))
# a backend asked for by name must build, unless WEB=0 says otherwise
WEB ?= $(if $(WEB_MISSING),$(if $(filter legacy platform,$(LORD_WEB_FORCED)),1,0),1)
ifeq ($(WEB),1)
ifneq ($(WEB_MISSING),)
$(error lord-web ($(LORD_WEB_BACKEND) backend) needs pkg-config packages that are missing: $(WEB_MISSING))
endif
endif

# the pure checks: argv, the registry, addresses and scope, the probe's answer, the icon check, download names, the wheel,
# the unread count, the user agent, the notification sound, dropped files, pasted text and file lists, the Edit commands and context menu items, the color scheme,
# the language of its own words, an app that is offline (weboffline.h: which load errors are the network's, the retry's delay, its page),
# the gate in front of the developer tools (webguard.h: the request head, Host, Origin, the one-time address and the cookie)
WEB_PURE_SRC := src/web/webargs.c src/web/webreg.c src/web/weburl.c src/web/webmanifest.c src/web/webdl.c src/web/webaxis.c src/web/webpage.c \
                src/web/webdrop.c src/web/webclip.c src/web/webedit.c src/web/websearch.c src/web/webscheme.c src/web/webinspect.c src/web/webguard.c src/web/webua.c src/web/webcall.c src/spellang.c src/sec/json.c
WEB_PURE_HDR := src/web/webargs.h src/web/webreg.h src/web/weburl.h src/web/webmanifest.h src/web/webdl.h src/web/webaxis.h src/web/webpage.h \
                src/web/webdrop.h src/web/webclip.h src/web/webedit.h src/web/websearch.h src/web/webscheme.h src/web/webinspect.h src/web/webguard.h src/web/weblangid.h src/web/weboffline.h src/web/webframe.h src/web/webua.h src/web/webcall.h src/spellang.h src/sec/json.h

# Chrome's Client Hints and navigator.userAgentData from one table (webua.h), the call log's lines (webcall.h)
test: build/webua-test
build/webua-test: tests/webua_test.c $(WEB_PURE_SRC) $(WEB_PURE_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/webua_test.c $(WEB_PURE_SRC)
	./$@ || { rm -f $@; exit 1; }

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
                         %/fractional-scale-v1-protocol.c %/text-input-unstable-v3-protocol.c %/cursor-shape-v1-protocol.c %/tablet-v2-protocol.c %/primary-selection-unstable-v1-protocol.c,$(CLIENT_GEN)) \
                $(filter %.c,$(WEB_GEN))

ifeq ($(LORD_WEB_BACKEND),platform)
WEB_BACK_SRC := src/web/webback_platform.c
else
WEB_BACK_SRC := src/web/webback_legacy.c src/web/webshm.c
endif
WEB_SRC := src/web/webmain.c src/web/webengine.c src/web/webwin.c $(WEB_BACK_SRC) src/web/webbus.c src/web/webtray.c src/web/webprobe.c \
           src/web/weblang.c src/web/webxfer.c src/web/webselect.c src/web/webdark.c src/web/webproxy.c src/web/webhints.c src/web/webcalls.c src/web/webmenu.c $(WEB_PURE_SRC) src/sec/cgroup.c src/sec/sandbox.c
WEB_HDR := src/web/web.h src/web/webwin.h src/web/webback.h src/web/webshm.h $(WEB_PURE_HDR) src/sec/cgroup.h src/sec/sandbox.h

# WebKit's headers are not ours: -isystem, so -Wall -Wextra apply to lord-web's own code only
WEB_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(WEB_PKGS)))
WEB_LIBS := $(shell pkg-config --libs $(WEB_PKGS))

# the script that answers the page's prefers-color-scheme (src/web/webscheme.js, tests/webscheme_test.mjs) as a C string
build/gen/webscheme_js.h: src/web/webscheme.js
	@mkdir -p build/gen
	@{ printf '/* generated from src/web/webscheme.js by mk/web.mk */\nstatic const char WEBSCHEME_JS[] =\n'; \
	   sed -e '/^[[:space:]]*\/\//d' -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $<; printf ';\n'; } > $@.new && mv $@.new $@

# navigator.userAgentData (src/web/webua.js) and the call log's page side (src/web/webcall.js), the same way
build/gen/webua_js.h: src/web/webua.js
	@mkdir -p build/gen
	@{ printf '/* generated from src/web/webua.js by mk/web.mk */\nstatic const char WEBUA_JS[] =\n'; \
	   sed -e '/^[[:space:]]*\/\//d' -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $<; printf ';\n'; } > $@.new && mv $@.new $@
build/gen/webcall_js.h: src/web/webcall.js
	@mkdir -p build/gen
	@{ printf '/* generated from src/web/webcall.js by mk/web.mk */\nstatic const char WEBCALL_JS[] =\n'; \
	   sed -e '/^[[:space:]]*\/\//d' -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $<; printf ';\n'; } > $@.new && mv $@.new $@

# the backend built last time: a switch (LORD_WEB_BACKEND) rebuilds lord-web and the backend test
build/gen/web_backend: FORCE
	@mkdir -p build/gen
	@echo "$(LORD_WEB_BACKEND)" | cmp -s - $@ || echo "$(LORD_WEB_BACKEND)" > $@

build/lord-web: $(WEB_SRC) $(WEB_HDR) $(CLIENT_GEN) $(WEB_GEN) build/obj/ui/stb.o build/gen/lord_version.h build/gen/webscheme_js.h build/gen/webua_js.h build/gen/webcall_js.h build/gen/web_backend
	@mkdir -p build
	@echo "lord-web: $(LORD_WEB_BACKEND) backend"
	$(CC) $(CPPFLAGS) -Ibuild/gen/client $(WEB_CFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(WEB_SRC) $(WEB_CLIENT_C) build/obj/ui/stb.o $(WEB_LIBS) -lm

# lord-web's web process extension (src/web/webext.c): Chrome's Client Hints on the page's requests. WebKit loads it
# from build/lord-web-ext/ next to lord-web (/usr/lib/lordos/bin/lord-web-ext/ once installed), never from elsewhere
ALL += build/lord-web-ext/lord-web-ext.so
all: build/lord-web-ext/lord-web-ext.so
test: build/lord-web-ext/lord-web-ext.so
build/lord-web-ext/lord-web-ext.so: src/web/webext.c src/web/webua.c src/web/webua.h src/web/webpage.h
	@mkdir -p build/lord-web-ext
	$(CC) $(CPPFLAGS) $(patsubst -I%,-isystem %,$(shell pkg-config --cflags wpe-web-process-extension-2.0)) $(CFLAGS) $(WARN) -std=c11 -fPIC -shared \
	    -Wl,-z,now -Wl,-z,relro -o $@ src/web/webext.c src/web/webua.c $(shell pkg-config --libs wpe-web-process-extension-2.0)

# Chrome's identity (Client Hints, userAgentData) and the call log on the real engine, headless (WPEPlatform only), driven
# by tests/webcalls_check.py (run by tests/run_suites.sh, and by tools/pkg/wpewebkit-chroot.sh's test with WebRTC)
ifeq ($(LORD_WEB_BACKEND),platform)
build/webcalls-check: tests/webcalls_check.c src/web/webhints.c src/web/webcalls.c $(WEB_PURE_SRC) $(WEB_PURE_HDR) src/web/web.h \
                      build/gen/lord_version.h build/gen/webua_js.h build/gen/webcall_js.h build/lord-web-ext/lord-web-ext.so
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(WEB_CFLAGS) $(patsubst -I%,-isystem %,$(shell pkg-config --cflags wpe-platform-headless-2.0)) $(CFLAGS) $(WARN) -std=c11 \
	    -o $@ tests/webcalls_check.c src/web/webhints.c src/web/webcalls.c $(WEB_PURE_SRC) $(WEB_LIBS) $(shell pkg-config --libs wpe-platform-headless-2.0) -lm
test: build/webcalls-check
endif

# what the context menu takes from WebKit, headless (tests/web_menu_check.c): spelling suggestions with the dictionaries
# spellang.h picks, applied after the menu; WPEPlatform only (run by tests/run_suites.sh when a dictionary is installed)
ifeq ($(LORD_WEB_BACKEND),platform)
build/web-menu-check: tests/web_menu_check.c $(WEB_BACK_SRC) src/web/webback.h src/web/websearch.c src/web/websearch.h src/spellang.c src/spellang.h src/sec/json.c build/gen/web_backend
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(WEB_CFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/web_menu_check.c $(WEB_BACK_SRC) src/web/websearch.c src/spellang.c src/sec/json.c $(WEB_LIBS) -lm
test: build/web-menu-check
endif

# the backend alone, headless (tests/web_backend_test.c): a local page rendered and its pixels checked, a key and a
# click that reach the page's listeners, a resize and a scale; run by tests/run_suites.sh (WebKit's sandbox: bwrap)
build/web-backend-test: tests/web_backend_test.c $(WEB_BACK_SRC) src/web/webback.h src/web/webshm.h src/web/webframe.h src/web/webaxis.h build/gen/web_backend
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(WEB_CFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/web_backend_test.c $(WEB_BACK_SRC) $(WEB_LIBS) -lm
test: build/web-backend-test
else
all: web-skipped
.PHONY: web-skipped
web-skipped:
ifeq ($(LORD_WEB_BACKEND),none)
	@echo "lord-web skipped: neither backend's packages are there (WPEPlatform: $(WEB_PLATFORM_PKGS); legacy: $(WEB_LEGACY_PKGS));" \
	      "missing for legacy: $(WEB_MISSING) (libwpewebkit-2.0-dev, libwpebackend-fdo-1.0-dev, libegl-dev; docs/VM-SETUP.md)"
else
	@echo "lord-web skipped: WEB=0 was given ($(LORD_WEB_BACKEND) backend would be built)"
endif
endif
