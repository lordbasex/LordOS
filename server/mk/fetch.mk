# lord-fetch (0.8.5, docs/FIREWALL.md "Blocklists"): the sandboxed helper that downloads the
# subscribed blocklists over HTTPS and parses them, and (0.8.8.10) the weather widget's forecast. With lord-sessiond and lord-maild, the only
# binaries that link OpenSSL; built when its headers are there (MAILNET, mk/mail.mk).
FETCH_SRC := src/fetch/fetch.c src/fetch/weather.c src/fetch/flathub.c src/fetch/fhget.c src/fpak.c src/fw/fwlists.c src/fw/rules.c src/fw/fwmap.c src/mail/http.c src/mail/net.c \
             src/doc/wire.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
MAILNET ?= $(shell pkg-config --exists openssl 2>/dev/null && echo 1 || echo 0)
ifeq ($(MAILNET),1)
all: build/lord-fetch
endif
build/lord-fetch: $(FETCH_SRC) src/fetch/fetchproto.h src/fetch/weather.h src/fetch/flathub.h src/fetch/fhget.h src/fpak.h src/fw/fwlists.h src/mail/http.h src/mail/net.h src/sec/sandbox.h
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
# the weather widget's request path and strict parsing (fetch/weather.c), no network
test: build/weather-test
build/weather-test: tests/weather_test.c src/fetch/weather.c src/fetch/weather.h src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/weather_test.c src/fetch/weather.c src/sec/json.c -lm
	./$@ || { rm -f $@; exit 1; }

# the weather widget's offline city list (assets/geo/cities.tsv, tools/geo/fetch-cities.sh)
test: build/geocities-test
build/geocities-test: tests/geocities_test.c src/fetch/geocities.c src/fetch/geocities.h src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/geocities_test.c src/fetch/geocities.c src/sec/json.c -lm
	./$@ || (rm -f $@; exit 1)

# the weather widget's table of places (fetch/wxsvc.c): shared by widgets on one city, the least recent one makes room
# (0.8.9.1, #193); no network, no lord-fetch
test: build/wxsvc-test
build/wxsvc-test: tests/wxsvc_test.c src/fetch/wxsvc.c src/fetch/wxsvc.h src/fetch/fetchspawn.c src/fetch/fetchspawn.h src/fetch/geocities.c src/fetch/weather.c src/hw/night.c src/doc/wire.c src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/wxsvc_test.c src/fetch/wxsvc.c src/fetch/fetchspawn.c src/fetch/geocities.c src/fetch/weather.c src/hw/night.c src/doc/wire.c src/sec/json.c -lm -lpthread
	./$@ || (rm -f $@; exit 1)

# the Store's rankings from Flathub's API (E10, docs/FLATPAK.md): the closed list of paths, the date and the strict
# parsing into app ids, on recorded answers (tests/data/flathub/api-2026-11-22) and hostile ones; no network
test: build/flathub-rank-test
build/flathub-rank-test: tests/flathub_rank_test.c src/fetch/flathub.c src/fetch/flathub.h src/fpak.c src/fpak.h src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/flathub_rank_test.c src/fetch/flathub.c src/fpak.c src/sec/json.c -lm
	./$@ || (rm -f $@; exit 1)
# lord-server's side: once a UTC day, the wait after failures, the cache read back strictly; no network, no lord-fetch
test: build/fhsvc-test
build/fhsvc-test: tests/fhsvc_test.c src/fetch/fhsvc.c src/fetch/fhsvc.h src/fetch/fetchspawn.c src/fetch/flathub.c src/fpak.c src/doc/wire.c src/sec/json.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/fhsvc_test.c src/fetch/fhsvc.c src/fetch/fetchspawn.c src/fetch/flathub.c src/fpak.c src/doc/wire.c src/sec/json.c -lm -lpthread
	./$@ || (rm -f $@; exit 1)
# lord-fetch's FQ_FLATHUB job: the exchange against a stand-in server (path, no redirect, 1 MB cap) and the helper's
# refusals; needs OpenSSL like lord-fetch
ifeq ($(MAILNET),1)
test: build/flathub-fetch-test
build/flathub-fetch-test: tests/flathub_fetch_test.c src/fetch/fhget.c src/fetch/fhget.h src/fetch/flathub.c src/fpak.c src/mail/http.c src/mail/net.c src/doc/wire.c src/sec/json.c build/lord-fetch
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/flathub_fetch_test.c src/fetch/fhget.c src/fetch/flathub.c src/fpak.c src/mail/http.c src/mail/net.c src/doc/wire.c src/sec/json.c $(shell pkg-config --libs openssl) -lm -lpthread
	./$@ build/lord-fetch || (rm -f $@; exit 1)
endif
