# Databases (0.8.8.7, docs/DATABASES.md): MySQL and MariaDB. lord-sql is the sandboxed helper that speaks the
# protocol, one process per connection; with lord-sessiond, lord-maild, lord-fetch and lord-tls, the only binaries
# that link OpenSSL (TLS starts inside the MySQL handshake; caching_sha2_password without TLS needs RSA). Built
# when its headers are there (MAILNET, mk/mail.mk). lord-server's side (sql/sqlc.c, lord.sql in ui/js_sql.c)
# reads the favorites, connects the socket and starts it; it links no OpenSSL.
SQL_HDR := src/sql/sqlc.h src/sql/sqlproto.h src/sql/mysqlproto.h src/doc/wire.h src/doc/proto.h src/sec/json.h src/fsops.h
SQLC_OBJ := build/obj/sql/sqlc.o build/obj/ui/js_sql.o
build/lord-server: $(SQLC_OBJ)
$(SQLC_OBJ): $(SQL_HDR) src/ui/js_sql.h
build/obj/ui/js.o: src/ui/js_sql.h

LSQL_SRC := src/sql/lordsql.c src/sql/mysqlproto.c src/doc/wire.c src/sec/json.c src/sec/sandbox.c src/sec/cgroup.c
MAILNET ?= $(shell pkg-config --exists openssl 2>/dev/null && echo 1 || echo 0)
ifeq ($(MAILNET),1)
all: build/lord-sql
endif
build/lord-sql: $(LSQL_SRC) $(SQL_HDR) src/sec/sandbox.h src/sec/cgroup.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(LSQL_SRC) $(shell pkg-config --libs openssl) -lpthread

# favorites.json: the validator (no secrets, ports, hosts, sizes), the lookup and the socket rule (tests/sqlc_test.c)
test: build/sqlc-test
SQLC_TEST_SRC := tests/sqlc_test.c src/sql/sqlc.c src/doc/wire.c src/sec/json.c src/fsops.c $(NETCONN_SRC)
build/sqlc-test: $(SQLC_TEST_SRC) $(SQL_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(SQLC_TEST_SRC) -lpthread
	./$@ || { rm -f $@; exit 1; }

# the protocol parsers against hostile servers (tests/sql_proto_test.c) and lord-sql itself against a fake
# MySQL server over a socketpair (tests/sql_e2e_test.c): handshake, both auth plugins, RSA, TLS, results
ifeq ($(MAILNET),1)
test: build/sql-proto-test build/sql-e2e-test
endif
build/sql-proto-test: tests/sql_proto_test.c src/sql/mysqlproto.c $(SQL_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ tests/sql_proto_test.c src/sql/mysqlproto.c $(shell pkg-config --libs openssl)
	./$@ || { rm -f $@; exit 1; }
build/sql-proto-test-asan: tests/sql_proto_test.c src/sql/mysqlproto.c $(SQL_HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 -o $@ tests/sql_proto_test.c src/sql/mysqlproto.c $(shell pkg-config --libs openssl)
	./$@
SQLE2E_SRC := tests/sql_e2e_test.c src/sql/mysqlproto.c src/sql/sqlc.c src/fsops.c src/doc/wire.c src/sec/json.c $(NETCONN_SRC)
build/sql-e2e-test: $(SQLE2E_SRC) $(SQL_HDR) build/lord-sql
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ $(SQLE2E_SRC) $(shell pkg-config --libs openssl) -lpthread
	./$@ build/lord-sql || { rm -f $@; exit 1; }
.PHONY: sql-asan
sql-asan: build/sql-proto-test-asan
