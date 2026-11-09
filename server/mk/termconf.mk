# The Terminal's configuration file (0.8.8.7, docs/TERMINAL.md "Configuration file"): termconf.c writes it atomically
# with its backups and protects it from lord.fs wherever the user put it; lord.termconf in ui/js_termconf.c.
TERMCONF_OBJ := build/obj/termconf.o build/obj/ui/js_termconf.o
build/lord-server: $(TERMCONF_OBJ)
$(TERMCONF_OBJ): src/termconf.h src/fsops.h src/ui/js_termconf.h
build/obj/ui/js.o: src/ui/js_termconf.h
build/obj/ui/js_sys.o: src/termconf.h

# paths, the default and a chosen file, unavailable places, atomic writes, backups, the migration, protection
test: build/termconf-test
build/termconf-test: tests/termconf_test.c src/termconf.c src/fsops.c src/termconf.h src/fsops.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/termconf_test.c src/termconf.c src/fsops.c
	./$@ || { rm -f $@; exit 1; }
