# Files → Network (docs/FILES.md, "Network"): lord-lan, the sandboxed helper that reads what the local
# network answers (mDNS, SMB2, ONC RPC), and lord-server's side (src/lan: one bounded pass on a thread,
# lord.lan in ui/js_lan.c, the share-* actions). libc only; our own code, no libsmbclient or libnfs.
LAN_SRV_SRC := src/lan/lan.c src/lan/rconnect.c src/lan/rconnect_run.c src/lan/rconnect_pure.c src/lan/lanaddr.c src/ui/js_lan.c src/ui/js_rdesk.c
LAN_SRV_OBJ := $(LAN_SRV_SRC:src/%.c=build/obj/%.o)
$(LAN_SRV_OBJ): $(wildcard src/lan/*.h) src/rdesk/proto.h src/ui/js_rdesk.h src/doc/wire.h src/print/mdns.h src/sec/json.h src/ui/js_lan.h src/admin.h src/errcode.h
build/lord-server: $(LAN_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_lan.h src/ui/js_rdesk.h

LAN_HELPER_SRC := src/lan/lanhelper.c src/lan/smbauth.c src/pkg/crypto.c src/lan/lanparse.c src/lan/lanaddr.c src/print/mdns.c src/doc/wire.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
all: build/lord-lan
build/lord-lan: $(LAN_HELPER_SRC) $(wildcard src/lan/*.h) src/pkg/crypto.h src/doc/wire.h src/doc/proto.h src/print/mdns.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DMDNS_PARSE_ONLY $(CFLAGS) $(WARN) -std=c11 -o $@ $(LAN_HELPER_SRC) -lpthread -lm
	chmod g-w $@

# the parsers against crafted and damaged answers, the address policy, the actions' arguments
test: build/lan-test
LAN_TEST_SRC := tests/lan_test.c src/lan/rconnect_pure.c src/lan/smbauth.c src/pkg/crypto.c src/lan/lanparse.c src/lan/lanaddr.c src/print/mdns.c
build/lan-test: $(LAN_TEST_SRC) $(wildcard src/lan/*.h) src/print/mdns.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DMDNS_PARSE_ONLY $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ $(LAN_TEST_SRC)
	./$@ || { rm -f $@; exit 1; }

# the programs rconnect runs: deadline, read-only system, no TCP when offline
test: build/rconnect-run-test
RCONNECT_RUN_SRC := tests/rconnect_run_test.c src/lan/rconnect_run.c src/sec/sandbox.c src/sec/cgroup.c
build/rconnect-run-test: $(RCONNECT_RUN_SRC) src/lan/rconnect.h src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(RCONNECT_RUN_SRC) -lpthread -lm
	./$@ || { rm -f $@; exit 1; }

# one real pass on the network this machine is on, printed as JSON (a tool, not part of make test):
#   build/lan-scan   (not in a LordOS session: there are no sockets there without a connector). Built with
#   LAN_ANY_LINK, so a test server behind a veth on an isolated network namespace counts as a link.
build/lan-scan: tests/lan_scan.c src/lan/lan.c src/lan/lanaddr.c src/print/mdns.c src/doc/wire.c src/sec/json.c src/sec/netconn.c src/sec/cgroup.c build/lord-lan
	$(CC) $(CPPFLAGS) -DMDNS_PARSE_ONLY -DLAN_ANY_LINK $(CFLAGS) $(WARN) -std=c11 -o $@ tests/lan_scan.c src/lan/lan.c src/lan/lanaddr.c src/print/mdns.c src/doc/wire.c src/sec/json.c src/sec/netconn.c src/sec/cgroup.c src/fw/rules.c src/fw/fwmap.c -lpthread -lm

# the share-* actions alone, real mounts as root on a test machine (a tool, not part of make test)
build/share-cli: tests/share_cli.c src/lan/share_admin.c src/lan/lanaddr.c src/lan/share_admin.h src/lan/lanaddr.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/share_cli.c src/lan/share_admin.c src/lan/lanaddr.c

# lord-lan's sign-in against one test server (a tool, not part of make test)
build/lan-list: tests/lan_list.c src/doc/wire.c build/lord-lan
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/lan_list.c src/doc/wire.c

# lord-rdesk (docs/FILES.md, "Remote desktop"): one RDP or VNC connection, sandboxed, drawing into a picture the engine
# shows. Our own RFB client; libfreerdp for RDP, its client library for shared folders only, no display, no toolkit.
RDESK_SRC := src/rdesk/rdeskhelper.c src/rdesk/rdeskrdp.c src/rdesk/rfb.c src/doc/wire.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
RDESK ?= $(shell pkg-config --exists freerdp3 freerdp-client3 winpr3 2>/dev/null && echo 1 || echo 0)
RDP_CFLAGS = $(shell pkg-config --cflags freerdp3 freerdp-client3 winpr3)
RDP_LIBS = $(shell pkg-config --libs freerdp3 freerdp-client3 winpr3)
ifeq ($(RDESK),1)
all: build/lord-rdesk
endif
build/lord-rdesk: $(RDESK_SRC) $(wildcard src/rdesk/*.h) src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RDP_CFLAGS) $(WARN) -Wno-deprecated-declarations -std=c11 -o $@ $(RDESK_SRC) -lpthread -lm $(RDP_LIBS)
	chmod g-w $@

# the RFB client against crafted and damaged servers
test: build/rdesk-test
build/rdesk-test: tests/rdesk_test.c src/rdesk/rfb.c src/rdesk/rfb.h src/rdesk/proto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ tests/rdesk_test.c src/rdesk/rfb.c
	./$@ || { rm -f $@; exit 1; }

# a server that offers only RDP's own security is refused before any credential
ifeq ($(RDESK),1)
test: build/rdesk-rdp-test
endif
build/rdesk-rdp-test: tests/rdesk_rdp_test.c src/rdesk/rdeskrdp.c src/rdesk/proto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RDP_CFLAGS) $(WARN) -Wno-deprecated-declarations -std=c11 -o $@ tests/rdesk_rdp_test.c src/rdesk/rdeskrdp.c -lpthread $(RDP_LIBS)
	./$@ || { rm -f $@; exit 1; }
