# The machines command (src/cli, docs/VIRTUALIZATION.md "From the terminal"; approved by the maintainer 2026-11-21):
# Machines from the terminal. libc only: lord-vm on the library's folders, and the session channel (vm/vmcli.c) for
# start and open. Installed as /usr/lib/lordos/bin/machines with /usr/bin/machines a link to it (tools/iso/mkdeb.sh).
MCLI_SRC := src/cli/machines.c src/cli/mcli.c src/vm/vmcli.c src/vm/vmexec.c src/vm/machine.c src/vm/vmgroup.c src/sec/json.c src/doc/wire.c src/pkg/crypto.c src/fsops.c
MCLI_HDR := src/cli/mcli.h src/vm/vmcli.h src/vm/vmexec.h src/vm/vmproto.h src/pkg/crypto.h src/fsops.h src/vm/machine.h src/sec/json.h src/doc/wire.h src/web/weblangid.h
all: build/machines
build/machines: $(MCLI_SRC) $(MCLI_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(MCLI_SRC) -lpthread

# sizes, names, set's options, run.json's states, the catalog's placeholders, and the session channel's request and
# answer lines (strict, with a fuzz pass), under ASan and UBSan
test: build/mcli-test
build/mcli-test: tests/mcli_test.c src/cli/mcli.c src/vm/vmcli.c src/sec/json.c $(MCLI_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/mcli_test.c src/cli/mcli.c src/vm/vmcli.c src/sec/json.c
	./$@ || { rm -f $@; exit 1; }
