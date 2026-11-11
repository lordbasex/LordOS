# Web apps, lord-server's side (0.8.8.8, docs/WEBAPPS.md): lord.webapps (ui/js_webapps.c), the registry
# (web/webreg.c, shared with lord-web), the probe and the lord-web processes (web/webmgr.c) and the permission
# questions on the session bus (ui/webask.c, on portal.c's connection). No WebKit here: only lord-web links it.
WEBAPPS_HDR := src/web/webreg.h src/web/weburl.h src/web/webmgr.h src/ui/webask.h src/ui/js_webapps.h src/sec/json.h
WEBAPPS_OBJ := build/obj/web/webreg.o build/obj/web/weburl.o build/obj/web/webmgr.o build/obj/ui/webask.o build/obj/ui/js_webapps.o
build/lord-server: $(WEBAPPS_OBJ)
$(WEBAPPS_OBJ): $(WEBAPPS_HDR)
build/obj/ui/js.o build/obj/ui/portal.o: $(WEBAPPS_HDR)

# the registry's rules (ids, names, URLs, unknown keys, the probe's answers, icons), the writes and the probe protocol
# against a stand-in lord-web (tests/webreg_test.c)
test: build/webreg-test
WEBREG_TEST_SRC := tests/webreg_test.c src/web/webreg.c src/web/weburl.c src/web/webmgr.c src/sec/json.c
build/webreg-test: $(WEBREG_TEST_SRC) $(WEBAPPS_HDR) ../assets/webapps/catalog.json
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -o $@ $(WEBREG_TEST_SRC) -lpthread
	./$@ || { rm -f $@; exit 1; }
