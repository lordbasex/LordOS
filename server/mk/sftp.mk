# The Terminal's Files mode (0.8.8.7, docs/TERMINAL.md "Files mode"): SFTP over the tab's own ssh connection.
# lord-server opens the channel on the ssh master's control socket (term/sshmux.c, OpenSSH's mux protocol: no
# program run) and starts lord-sftp, the sandboxed helper that speaks SFTP (term/sftp.c) on the passed fds.
# No library beyond libc.
SFTP_HDR := src/term/sshmux.h src/term/sftp.h src/term/sftpproto.h src/doc/wire.h src/doc/proto.h src/sec/json.h

LSFTP_SRC := src/term/lordsftp.c src/term/sftp.c src/term/sshmux.c src/doc/wire.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
all: build/lord-sftp
build/lord-sftp: $(LSFTP_SRC) $(SFTP_HDR) src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LSFTP_SRC) -lpthread

# the mux and SFTP wire against hostile bytes (tests/sftp_test.c), lord-sftp against a fake server over pipes
test: build/sftp-test
SFTP_TEST_SRC := tests/sftp_test.c src/term/sftp.c src/term/sshmux.c src/doc/wire.c src/sec/json.c
build/sftp-test: $(SFTP_TEST_SRC) $(SFTP_HDR) build/lord-sftp
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(SFTP_TEST_SRC) -lpthread
	./$@ build/lord-sftp || { rm -f $@; exit 1; }
build/sftp-test-asan: $(SFTP_TEST_SRC) $(SFTP_HDR) build/lord-sftp
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ $(SFTP_TEST_SRC) -lpthread
	./$@ build/lord-sftp
