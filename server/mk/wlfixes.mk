# The core protocol's wl_fixes global (src/wl/wlfixes.c, docs/WAYLAND.md): libwayland 1.24 ships wl_fixes_interface;
# on an older one (Debian 13: 1.23) wayland-scanner generates it from third_party/wayland/wl-fixes.xml, the interface
# vendored from wayland.xml 1.24, for lord-server and for the test client (tests/wlfixes_client.c).
WLFIXES_XML := ../third_party/wayland/wl-fixes.xml
WLFIXES_SYS := $(shell pkg-config --atleast-version=1.24 wayland-server 2>/dev/null && echo 1)
WLFIXES_OBJ := build/obj/wl/wlfixes.o
build/lord-server: $(WLFIXES_OBJ)
ifeq ($(WLFIXES_SYS),)
WLFIXES_CLIENT := build/gen/client/wl-fixes-protocol.c
$(WLFIXES_OBJ): CPPFLAGS += -DLORD_WLFIXES_VENDORED
$(WLFIXES_OBJ): build/gen/wl-fixes-protocol.h
build/lord-server: build/gen/wl-fixes-protocol.o
build/gen/wl-fixes-protocol.h: $(WLFIXES_XML)
	@mkdir -p build/gen
	wayland-scanner server-header $< $@
build/gen/wl-fixes-protocol.c: $(WLFIXES_XML)
	@mkdir -p build/gen
	wayland-scanner private-code $< $@
build/gen/client/wl-fixes-client-protocol.h: $(WLFIXES_XML)
	@mkdir -p build/gen/client
	wayland-scanner client-header $< $@
build/gen/client/wl-fixes-protocol.c: $(WLFIXES_XML)
	@mkdir -p build/gen/client
	wayland-scanner private-code $< $@
build/wlfixes-client: build/gen/client/wl-fixes-client-protocol.h
endif

# run by tests/wlperf_headless.sh ("fixes") against a headless lord-server
test: build/wlfixes-client
build/wlfixes-client: tests/wlfixes_client.c $(WLFIXES_CLIENT)
	@mkdir -p build
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(if $(WLFIXES_SYS),,-DLORD_WLFIXES_VENDORED -Ibuild/gen/client) $(WARN) -std=c11 -o $@ tests/wlfixes_client.c $(WLFIXES_CLIENT) -lwayland-client
