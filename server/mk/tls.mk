# lord-tls (0.8.5, docs/PACKAGES.md "HTTPS"): TLS for the package engine's downloads. The download
# child (nobody) connects the socket and hands it over; lord-tls verifies the server's certificate and
# host name, applies the helpers' sandbox and relays plain HTTP. With lord-sessiond, lord-maild and
# lord-fetch, the only binaries that link OpenSSL; built when its headers are there.
TLS_SRC := src/tls/tlsproxy.c src/sec/sandbox.c src/sec/cgroup.c
ifeq ($(MAILNET),1)
ifeq ($(PROFILE),os)
all: build/lord-tls
endif
endif
build/lord-tls: $(TLS_SRC) src/sec/sandbox.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(TLS_SRC) $(shell pkg-config --libs openssl) -lpthread
