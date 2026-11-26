# Hands (docs/HANDS.md; approved by the maintainer on 2026-11-26): an AI agent uses the LordOS desktop through lord-server.
# lord-server's side: the policy and state (src/hands/hands.c), the screenshots' PNG (handsimg.c, vendored stb), the
# session-bus service org.lordos.Hands1 (src/ui/handsbus.c, on portal.c's connection) and lord.hands (src/ui/js_hands.c).
# libc only.
HANDS_SRV_SRC := src/hands/hands.c src/hands/handsimg.c src/hands/handsmark.c src/ui/handsbus.c src/ui/js_hands.c
HANDS_SRV_OBJ := $(HANDS_SRV_SRC:src/%.c=build/obj/%.o)
HANDS_HDR := src/hands/hands.h src/hands/handsimg.h src/hands/handsmark.h src/ui/handsbus.h src/ui/js_hands.h src/vault/dbus.h src/sec/json.h
$(HANDS_SRV_OBJ): $(HANDS_HDR)
build/lord-server: $(HANDS_SRV_OBJ)
build/obj/main.o build/obj-testhooks/main.o build/obj/ui/js.o build/obj/ui/portal.o build/obj/remote/mirror.o: $(HANDS_HDR)

# the policy against a fake host, the bus's arguments and a fuzz of them, a stand-in lord-server on a private
# dbus-daemon (the same user served, another refused), under ASan and UBSan (docs/HANDS.md "Tests")
test: build/hands-test
HANDS_TEST_SRC := tests/hands_test.c src/hands/hands.c src/hands/handsimg.c src/hands/handsmark.c src/hands/mcp.c src/vault/dbus.c src/sec/json.c
build/hands-test: $(HANDS_TEST_SRC) src/ui/handsbus.c $(HANDS_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -c -Dgetuid=hands_test_getuid -o build/handsbus-test.o src/ui/handsbus.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ $(HANDS_TEST_SRC) build/handsbus-test.o
	./$@ || { rm -f $@; exit 1; }

# lordos-mcp (src/cli/lordosmcp.c, docs/HANDS.md): the MCP server (stdio) for Claude Code and Lumo's agent; it only calls
# org.lordos.Hands1, or with --headless starts a private QA desktop (a private dbus-daemon and lord-server --hands-qa).
# libc only, plus vendored stb_image (PNG, for a recording's GIF). Installed as /usr/lib/lordos/bin/lordos-mcp with
# /usr/bin/lordos-mcp a link to it (tools/iso/mkdeb.sh).
LORDOS_MCP_SRC := src/cli/lordosmcp.c src/hands/mcp.c src/hands/mcpimg.c src/hands/hands.c src/hands/handsimg.c src/vault/dbus.c src/sec/json.c
all: build/lordos-mcp
build/lordos-mcp: $(LORDOS_MCP_SRC) src/hands/mcp.h src/hands/mcpimg.h $(HANDS_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(LORDOS_MCP_SRC) -lm
