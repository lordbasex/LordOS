# Settings' hardware pages (src/hw, docs/SETTINGS.md): displays (EDID, modes, Night Light),
# Bluetooth (BlueZ over our own D-Bus client), battery and power, date and time, default apps.
# Objects are added to lord-server as extra prerequisites ($^ in its link rule takes them).
HW_SRC := src/hw/edid.c src/hw/modes.c src/hw/night.c src/hw/mimeapps.c src/hw/power.c src/hw/jbuf.c src/hw/display.c \
          src/hw/uevent.c src/hw/sysbus.c src/hw/bt.c src/hw/ppd.c src/hw/hw.c src/hw/hw_admin.c src/bench/boottest.c src/hw/defaults.c src/hw/notifyd.c src/hw/hwscan.c src/hw/brframe.c src/ui/js_hw.c \
          src/sysreport/sysreport.c src/sysreport/sysreport_out.c src/sysreport/ids.c src/sysreport/smbios.c src/sysreport/facts.c src/hw/rpifw.c \
          src/ui/js_fan.c src/fan/fancurve.c
HW_HDR := $(wildcard src/hw/*.h) src/apps.h $(wildcard src/sysreport/*.h) $(wildcard src/fan/*.h) src/ui/js_hw.h src/ui/js_fan.h src/kms.h src/vault/dbus.h
HW_OBJ := $(HW_SRC:src/%.c=build/obj/%.o)
$(HW_OBJ): $(HW_HDR)
build/lord-server: $(HW_OBJ)
# pure parts: EDID, mode selection, gamma/temperature/sun math, mimeapps.list and globs2, power_supply,
# and the admin actions' validators
test: build/settings-hw-test
build/settings-hw-test: tests/settings_hw_test.c src/hw/edid.c src/hw/modes.c src/hw/night.c src/hw/mimeapps.c src/hw/power.c src/hw/hw_admin.c src/bench/boottest.c src/hw/hwscan.c src/sysreport/smbios.c src/hw/jbuf.c src/sec/json.c $(HW_HDR) src/bench/boottest.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/settings_hw_test.c src/hw/edid.c src/hw/modes.c src/hw/night.c src/hw/mimeapps.c src/hw/power.c src/hw/hw_admin.c src/bench/boottest.c src/hw/hwscan.c src/sysreport/smbios.c src/hw/jbuf.c src/sec/json.c -lm
	./$@ || { rm -f $@; exit 1; }
# BlueZ parsing (GetManagedObjects, PropertiesChanged, Battery1, paths, state JSON) without a bus
test: build/bt-test
BT_TEST_SRC := tests/bt_test.c src/hw/bt.c src/hw/sysbus.c src/hw/jbuf.c src/vault/dbus.c src/sec/json.c
build/bt-test: $(BT_TEST_SRC) $(HW_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(BT_TEST_SRC)
	./$@ || { rm -f $@; exit 1; }
# hardware facts for the Store's suggestions (hwscan.c) over fake sysfs/DMI/PCI/USB/kmsg trees;
# `build/hwscan-test dump [root]` prints the facts JSON of a tree (or of this machine)
test: build/hwscan-test
build/hwscan-test: tests/hwscan_test.c src/hw/hwscan.c src/hw/jbuf.c src/sec/json.c $(HW_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/hwscan_test.c src/hw/hwscan.c src/hw/jbuf.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
# Settings → Appearance → "Browsers: tabs in the title bar" (brframe.c): Chromium's Preferences edit
# (malformed JSON, missing keys, theme extensions, idempotence), Firefox's user.js line and tidy,
# running browsers, missing and hostile profile folders, over fake homes
test: build/brframe-test
build/brframe-test: tests/brframe_test.c src/hw/brframe.c src/hw/jbuf.c src/sec/json.c $(HW_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/brframe_test.c src/hw/brframe.c src/hw/jbuf.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
