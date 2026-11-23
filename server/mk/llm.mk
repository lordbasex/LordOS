# Sentinela (docs/SENTINELA.md): lord-server's side of the local model engine (src/llm/engine.c: started on demand,
# stopped when idle, its answers read on its own thread; facts.c: a process's origin), lord.sentinela
# (src/ui/js_sentinela.c), and lord-llm, the launcher that puts llama-server in its sandbox (launcher.c, llmjail.c).
# libc only: the engine is a separate program (LordOS's llama.cpp package), never linked into anything here.
LLM_SRV_SRC := src/llm/engine.c src/llm/facts.c src/ui/js_sentinela.c
LLM_SRV_OBJ := $(LLM_SRV_SRC:src/%.c=build/obj/%.o)
$(LLM_SRV_OBJ): src/llm/engine.h src/llm/llmjail.h src/ui/js_sentinela.h src/sec/json.h
build/lord-server: $(LLM_SRV_OBJ)
build/obj/ui/js.o: src/ui/js_sentinela.h

LLM_LAUNCHER_SRC := src/llm/launcher.c src/llm/llmjail.c src/sec/cgroup.c
all: build/lord-llm
build/lord-llm: $(LLM_LAUNCHER_SRC) src/llm/llmjail.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LLM_LAUNCHER_SRC)
	chmod g-w $@

# the engine with a stand-in for llama-server in the real sandbox: the socket, every escape refused, idling, facts
test: build/llm-engine-test
build/llm-fake-engine: tests/llm_fake_engine.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $<
	chmod g-w $@
LLM_TEST_SRC := tests/llm_engine_test.c src/llm/engine.c src/llm/facts.c src/sec/json.c
build/llm-engine-test: $(LLM_TEST_SRC) src/llm/engine.h src/llm/llmjail.h src/sec/json.h build/lord-llm build/llm-fake-engine
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LLM_TEST_SRC) -lpthread
	./$@ || { rm -f $@; exit 1; }
