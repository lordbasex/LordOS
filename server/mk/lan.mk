# Files → Network (docs/FILES.md, "Network"): lord-lan, the sandboxed helper that reads what the local
# network answers (mDNS, SMB2, ONC RPC), and lord-server's side (src/lan: one bounded pass on a thread,
# lord.lan in ui/js_lan.c, the share-* actions). libc only; our own code, no libsmbclient or libnfs.
LAN_SRV_SRC := src/lan/lan.c src/lan/lanaddr.c src/lan/share_admin.c src/ui/js_lan.c
LAN_SRV_OBJ := $(LAN_SRV_SRC:src/%.c=build/obj/%.o)
$(LAN_SRV_OBJ): $(wildcard src/lan/*.h) src/doc/wire.h src/print/mdns.h src/sec/json.h src/ui/js_lan.h src/admin.h src/errcode.h
build/lord-server: $(LAN_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_lan.h
build/obj/admin.o: src/lan/share_admin.h

LAN_HELPER_SRC := src/lan/lanhelper.c src/lan/lanparse.c src/lan/lanaddr.c src/print/mdns.c src/doc/wire.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
all: build/lord-lan
build/lord-lan: $(LAN_HELPER_SRC) $(wildcard src/lan/*.h) src/doc/wire.h src/doc/proto.h src/print/mdns.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DMDNS_PARSE_ONLY $(CFLAGS) $(WARN) -std=c11 -o $@ $(LAN_HELPER_SRC) -lpthread -lm
	chmod g-w $@

# the parsers against crafted and damaged answers, the address policy, the actions' arguments
test: build/lan-test
LAN_TEST_SRC := tests/lan_test.c src/lan/lanparse.c src/lan/lanaddr.c src/print/mdns.c
build/lan-test: $(LAN_TEST_SRC) $(wildcard src/lan/*.h) src/print/mdns.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DMDNS_PARSE_ONLY $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ $(LAN_TEST_SRC)
	./$@ || { rm -f $@; exit 1; }

# one real pass on the network this machine is on, printed as JSON (a tool, not part of make test):
#   build/lan-scan   (not in a LordOS session: there are no sockets there without a connector). Built with
#   LAN_ANY_LINK, so a test server behind a veth on an isolated network namespace counts as a link.
build/lan-scan: tests/lan_scan.c src/lan/lan.c src/lan/lanaddr.c src/print/mdns.c src/doc/wire.c src/sec/json.c build/lord-lan
	$(CC) $(CPPFLAGS) -DMDNS_PARSE_ONLY -DLAN_ANY_LINK $(CFLAGS) $(WARN) -std=c11 -o $@ tests/lan_scan.c src/lan/lan.c src/lan/lanaddr.c src/print/mdns.c src/doc/wire.c src/sec/json.c -lpthread -lm

# the share-* actions alone, real mounts as root on a test machine (a tool, not part of make test)
build/share-cli: tests/share_cli.c src/lan/share_admin.c src/lan/lanaddr.c src/lan/share_admin.h src/lan/lanaddr.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/share_cli.c src/lan/share_admin.c src/lan/lanaddr.c
