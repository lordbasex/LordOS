# Mail, Calendar, Contacts and Reminders (docs/MAIL.md). lord-maild is the sandboxed helper that
# speaks IMAP, SMTP, CalDAV and CardDAV; with lord-sessiond it is the only binary linking OpenSSL.
MAIL_PARSE := src/mail/xml.c src/mail/mime.c src/mail/html.c src/mail/imapp.c src/mail/ical.c src/mail/vcard.c
MAIL_HDR := $(wildcard src/mail/*.h) src/doc/wire.h src/sec/json.h src/ui/tzif.h
# parsers: IMAP, MIME, HTML sanitizer, iCalendar, vCard and XML, with a small fuzzer (tests/mail_test.c)
test: build/mail-test
build/mail-test: tests/mail_test.c $(MAIL_PARSE) $(MAIL_HDR) src/doc/wire.c src/sec/json.c src/ui/tzif.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/mail_test.c $(MAIL_PARSE) src/doc/wire.c src/sec/json.c src/ui/tzif.c -lm
	./$@ || { rm -f $@; exit 1; }
# the same under AddressSanitizer + UBSan (not part of `test`: slower)
build/mail-test-asan: tests/mail_test.c $(MAIL_PARSE) $(MAIL_HDR) src/doc/wire.c src/sec/json.c src/ui/tzif.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) -std=c11 -o $@ tests/mail_test.c $(MAIL_PARSE) src/doc/wire.c src/sec/json.c src/ui/tzif.c -lm
	./$@
.PHONY: mail-asan
mail-asan: build/mail-test-asan
# lord-server's side: start lord-maild, its channel, the connector (allowed servers only) and lord.mail
MAILC_OBJ := build/obj/mail/mailc.o build/obj/ui/js_mail.o
build/lord-server: $(MAILC_OBJ)
$(MAILC_OBJ): $(wildcard src/mail/*.h) src/ui/js_mail.h
# the connector's policy: allowed endpoints and the public-address check for remote images
test: build/mailc-test
build/mailc-test: tests/mailc_test.c src/mail/mailc.c src/mail/mailc.h src/doc/wire.c src/sec/json.c $(NETCONN_SRC)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/mailc_test.c src/mail/mailc.c src/doc/wire.c src/sec/json.c $(NETCONN_SRC) -lpthread -lm
	./$@ || { rm -f $@; exit 1; }
# hostile servers: HTTP chunk sizes / Content-Length / header limits and IMAP literal sizes,
# over a socketpair (no network). Needs the OpenSSL headers (net.c), like lord-maild.
MAILNET ?= $(shell pkg-config --exists openssl 2>/dev/null && echo 1 || echo 0)
ifeq ($(MAILNET),1)
test: build/mailnet-test
endif
MAILNET_SRC := tests/mailnet_test.c src/mail/http.c src/mail/net.c src/mail/imapio.c
build/mailnet-test: $(MAILNET_SRC) src/mail/http.h src/mail/net.h src/mail/imapio.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(MAILNET_SRC) $(shell pkg-config --libs openssl)
	./$@ || { rm -f $@; exit 1; }
build/mailnet-test-asan: $(MAILNET_SRC) src/mail/http.h src/mail/net.h src/mail/imapio.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ $(MAILNET_SRC) $(shell pkg-config --libs openssl)
	./$@
mail-asan: build/mailnet-test-asan
# lord-maild: the sandboxed helper (IMAP, SMTP, CalDAV, CardDAV). With lord-sessiond, the only
# binary that links OpenSSL; built when its headers are there (MAILNET).
LMD_SRC := src/mail/maild.c src/mail/store.c src/mail/search.c src/mail/imap.c src/mail/imapio.c src/mail/smtp.c \
           src/mail/msgview.c src/mail/http.c src/mail/dav.c src/mail/pim.c src/mail/net.c src/mail/snpf.c $(MAIL_PARSE) \
           src/doc/wire.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c src/ui/tzif.c
ifeq ($(MAILNET),1)
all: build/lord-maild
endif
build/obj/mail/mimg.o: src/mail/mimg.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<
build/lord-maild: $(LMD_SRC) build/obj/mail/mimg.o $(MAIL_HDR) src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LMD_SRC) build/obj/mail/mimg.o $(shell pkg-config --libs openssl) -lpthread -lm
# manual driver for lord-maild (not in `make test`): build/maild-cli '{"op":"status"}' --wait 5
build/maild-cli: tests/maild_cli.c src/mail/mailc.c src/doc/wire.c src/sec/json.c src/mail/mailc.h $(NETCONN_SRC)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/maild_cli.c src/mail/mailc.c src/doc/wire.c src/sec/json.c $(NETCONN_SRC) -lpthread -lm
