# Hands (docs/HANDS.md; approved by the maintainer on 2026-11-26): an AI agent uses the LordOS desktop through lord-server.
# lord-server's side: the policy and state (src/hands/hands.c), the screenshots' PNG (handsimg.c, vendored stb), the
# session-bus service org.lordos.Hands1 (src/ui/handsbus.c, on portal.c's connection) and lord.hands (src/ui/js_hands.c).
# libc only.
HANDS_SRV_SRC := src/hands/hands.c src/hands/handsimg.c src/ui/handsbus.c src/ui/js_hands.c
HANDS_SRV_OBJ := $(HANDS_SRV_SRC:src/%.c=build/obj/%.o)
HANDS_HDR := src/hands/hands.h src/hands/handsimg.h src/ui/handsbus.h src/ui/js_hands.h src/vault/dbus.h src/sec/json.h
$(HANDS_SRV_OBJ): $(HANDS_HDR)
build/lord-server: $(HANDS_SRV_OBJ)
build/obj/main.o build/obj-testhooks/main.o build/obj/ui/js.o build/obj/ui/portal.o build/obj/remote/mirror.o: $(HANDS_HDR)
