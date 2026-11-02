# Several displays and HiDPI (docs/DISPLAYS.md): the layout math of the global logical space
# (src/outputs.c) is linked into axia-server and tested on its own.
build/axia-server: build/obj/outputs.o
build/obj/outputs.o: src/outputs.h
test: build/outputs-test
build/outputs-test: tests/outputs_test.c src/outputs.c src/outputs.h src/canvas.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/outputs_test.c src/outputs.c -lm
	./$@ || { rm -f $@; exit 1; }
