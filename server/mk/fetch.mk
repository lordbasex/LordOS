# lord-fetch (0.8.5, docs/FIREWALL.md "Blocklists"): the sandboxed helper that downloads the
# subscribed blocklists over HTTPS and parses them. With lord-sessiond and lord-maild, the only
# binaries that link OpenSSL; built when its headers are there (MAILNET, mk/mail.mk).
FETCH_SRC := src/fetch/fetch.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/mail/http.c src/mail/net.c \
             src/doc/wire.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
MAILNET ?= $(shell pkg-config --exists openssl 2>/dev/null && echo 1 || echo 0)
ifeq ($(MAILNET),1)
all: build/lord-fetch
endif
build/lord-fetch: $(FETCH_SRC) src/fetch/fetchproto.h src/fw/fwlists.h src/mail/http.h src/mail/net.h src/sec/sandbox.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(FETCH_SRC) $(shell pkg-config --libs openssl) -lpthread -lm
# subscriptions (fw/fwsub.c) without network, and lord-fetch's refusals (tests/fwsub_test.c)
FWSUB_SRC := src/fw/fwsub.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/doc/wire.c src/sec/json.c src/sec/netconn.c src/sec/cgroup.c
ifeq ($(MAILNET),1)
test: build/fwsub-test
build/fwsub-test: build/lord-fetch
endif
build/fwsub-test: tests/fwsub_test.c $(FWSUB_SRC) src/fw/fwsub.h src/fetch/fetchproto.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fwsub_test.c $(FWSUB_SRC) -lpthread
	./$@ build/lord-fetch || { rm -f $@; exit 1; }
