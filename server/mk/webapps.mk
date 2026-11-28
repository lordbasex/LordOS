# Web apps, lord-server's side (0.8.8.8, docs/WEBAPPS.md): lord.webapps (ui/js_webapps.c), the registry
# (web/webreg.c, shared with lord-web), the probe and the lord-web processes (web/webmgr.c) and the permission
# questions on the session bus (ui/webask.c, on portal.c's connection). No WebKit here: only lord-web links it.
WEBAPPS_HDR := src/web/webreg.h src/web/webedit.h src/web/webinspect.h src/web/webscheme.h src/web/websearch.h src/web/weburl.h src/web/webmgr.h src/web/webmem.h src/web/webwatch.h src/ui/webask.h src/ui/webalive.h src/ui/webbadge.h src/ui/webicon.h src/ui/js_webapps.h src/sec/json.h src/hw/notifyd.h
WEBAPPS_OBJ := build/obj/web/webreg.o build/obj/web/webedit.o build/obj/web/webinspect.o build/obj/web/websearch.o build/obj/web/webscheme.o build/obj/web/weburl.o build/obj/web/webmgr.o build/obj/web/webmem.o build/obj/web/webwatch.o build/obj/ui/webask.o build/obj/ui/webalive.o build/obj/ui/webbadge.o build/obj/ui/webicon.o build/obj/ui/js_webapps.o
build/lord-server: $(WEBAPPS_OBJ)
$(WEBAPPS_OBJ): $(WEBAPPS_HDR)
build/obj/ui/js.o build/obj/ui/portal.o build/obj/ui/js_wm.o: $(WEBAPPS_HDR)

# the registry's rules (ids, names, URLs, unknown keys, the probe's answers, icons), the writes and the probe protocol
# against a stand-in lord-web (tests/webreg_test.c)
test: build/webreg-test
WEBREG_TEST_SRC := tests/webreg_test.c src/web/webinspect.c src/web/websearch.c src/web/webreg.c src/web/weburl.c src/web/webmgr.c src/web/webmem.c src/web/webscheme.c src/sec/json.c src/lordlog.c
build/webreg-test: $(WEBREG_TEST_SRC) $(WEBAPPS_HDR) ../assets/webapps/catalog.json
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ $(WEBREG_TEST_SRC) -lpthread
	./$@ || { rm -f $@; exit 1; }

# lord.webapps.memory's sum (web/webmem.c): Pss of a lord-web and its descendants, on a made-up /proc tree
test: build/webmem-test
build/webmem-test: tests/webmem_test.c src/web/webmem.c src/web/webmem.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ tests/webmem_test.c src/web/webmem.c
	./$@ || { rm -f $@; exit 1; }

# SetBadge's checks (ui/webbadge.c: one checked sender per app and profile, the count clamped, a departure clears it)
# and the icon cache's names and files (ui/webicon.c) on a temporary home (tests/webdock_test.c)
test: build/webdock-test
WEBDOCK_TEST_SRC := tests/webdock_test.c src/ui/webbadge.c src/ui/webicon.c src/ui/stb.c src/web/webreg.c src/web/weburl.c src/sec/json.c
build/webdock-test: $(WEBDOCK_TEST_SRC) src/ui/webbadge.h src/ui/webicon.h src/web/webreg.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ $(WEBDOCK_TEST_SRC) -lm
	./$@ || { rm -f $@; exit 1; }

# when silence means a web app hangs (web/webwatch.c: lord-server's watch of lord-web, lord-web's of its page)
test: build/webwatch-test
build/webwatch-test: tests/webwatch_test.c src/web/webwatch.c src/web/webwatch.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ tests/webwatch_test.c src/web/webwatch.c
	./$@ || { rm -f $@; exit 1; }

# a web app that hangs is started again (ui/webalive.c, web/webmgr.c): end to end on a private session bus against a
# stand-in lord-web that stops answering (tests/webalive_fake.c); run by tests/run_suites.sh under dbus-run-session
test: build/webalive-test build/webalive-fake
WEBALIVE_TEST_SRC := tests/webalive_test.c src/ui/webalive.c src/web/webwatch.c src/web/webmgr.c src/web/webmem.c src/web/webreg.c \
                     src/web/webinspect.c src/web/websearch.c src/web/weburl.c src/web/webscheme.c src/vault/dbus.c src/sec/json.c src/lordlog.c
build/webalive-test: $(WEBALIVE_TEST_SRC) $(WEBAPPS_HDR) | build/webalive-fake
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ $(WEBALIVE_TEST_SRC) -lpthread
build/webalive-fake: tests/webalive_fake.c src/vault/dbus.c src/web/webreg.c src/web/weburl.c src/sec/json.c src/vault/dbus.h src/web/webreg.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -o $@ tests/webalive_fake.c src/vault/dbus.c src/web/webreg.c src/web/weburl.c src/sec/json.c
	@chmod go-w $@                       # web/webmgr.c refuses a lord-web anyone else may write (umask 0002 makes it g+w)
