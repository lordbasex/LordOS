# Logitech batteries (docs/DEVICES.md; maintainer's approval 2026-11-17, battery reads only): lord-hidpp, the
# setgid helper that alone may open a Logitech device's hidraw node (packaging/udev/72-lordos-hidpp.rules), asks
# HID++ for the battery and prints numbers only; lord-server's side runs it at most once a minute (lord.hidpp).
HIDPP_SRV_SRC := src/hw/hidppsvc.c
HIDPP_SRV_OBJ := $(HIDPP_SRV_SRC:src/%.c=build/obj/%.o)
$(HIDPP_SRV_OBJ): src/hw/hidppsvc.h
build/lord-server: $(HIDPP_SRV_OBJ)
build/obj/ui/js_hw.o: src/hw/hidppsvc.h

LORD_HIDPP_SRC := src/hw/lordhidpp.c src/hw/hidpp.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
build/lord-hidpp: $(LORD_HIDPP_SRC) src/hw/hidpp.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LORD_HIDPP_SRC) -lpthread
	chmod g-w $@
all: build/lord-hidpp

test: build/hidpp-test
build/hidpp-test: tests/hidpp_test.c src/hw/hidpp.c src/hw/hidpp.h src/hw/hidppsvc.c src/hw/hidppsvc.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/hidpp_test.c src/hw/hidpp.c src/hw/hidppsvc.c -lpthread
	./$@ || (rm -f $@; exit 1)

# a diagnostic build that tells every step on stderr; never installed (make build/lord-hidpp-debug, run with sudo)
build/lord-hidpp-debug: $(LORD_HIDPP_SRC) src/hw/hidpp.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DHIDPP_DEBUG $(CFLAGS) $(WARN) -std=c11 -o $@ $(LORD_HIDPP_SRC) -lpthread
