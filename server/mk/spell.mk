# The spell checker (docs/SPELLING.md; AGENTS.md "lord-spell"): lord-spell, the only program that loads Hunspell
# (libhunspell-1.7.so.0, dlopen'd from the fixed system folders, sandboxed), and lord-server's side: the service that
# starts it with the chosen dictionaries as descriptors and reads its answers on a thread (src/spell/spellsvc.c), the
# person's dictionary, and lord.spell (src/ui/js_spell.c). lord-server never links or loads Hunspell.
SPELL_HDR := src/spell/spellproto.h src/spell/spellsvc.h src/sec/sandbox.h src/sec/cgroup.h
SPELL_SRV_SRC := src/spell/spellproto.c src/spell/spelldict.c src/spell/spellsvc.c src/ui/js_spell.c
SPELL_SRV_OBJ := $(SPELL_SRV_SRC:src/%.c=build/obj/%.o)
$(SPELL_SRV_OBJ): $(SPELL_HDR) src/ui/js_spell.h src/sec/json.h
build/lord-server: $(SPELL_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_spell.h

LORD_SPELL_SRC := src/spell/lordspell.c src/spell/spellproto.c src/sec/sandbox.c src/sec/cgroup.c src/sec/json.c
build/lord-spell: $(LORD_SPELL_SRC) $(SPELL_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(LORD_SPELL_SRC) -ldl -lpthread
	chmod g-w $@
all: build/lord-spell

# Tests only, never installed: a helper that tries what its sandbox must refuse after loading (and answers only when
# all of it was refused), and one that answers a check with a malformed frame.
build/lord-spell-probe: $(LORD_SPELL_SRC) $(SPELL_HDR)
	$(CC) $(CPPFLAGS) -DSPELL_PROBE $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(LORD_SPELL_SRC) -ldl -lpthread
	chmod g-w $@
build/lord-spell-evil: $(LORD_SPELL_SRC) $(SPELL_HDR)
	$(CC) $(CPPFLAGS) -DSPELL_EVIL $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(LORD_SPELL_SRC) -ldl -lpthread
	chmod g-w $@
# the frames' parser fuzzed (ASan, UBSan), the person's dictionary, and the service end to end with the real helper,
# Hunspell and small dictionaries made by the test
test: build/spell-test
SPELL_TEST_SRC := tests/spell_proto_test.c src/spell/spellproto.c src/spell/spelldict.c src/spell/spellsvc.c src/sec/json.c
build/spell-test: $(SPELL_TEST_SRC) $(SPELL_HDR) build/lord-spell build/lord-spell-probe build/lord-spell-evil
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ $(SPELL_TEST_SRC) -lpthread
	./$@ build/lord-spell build/lord-spell-probe build/lord-spell-evil || { rm -f $@; exit 1; }
