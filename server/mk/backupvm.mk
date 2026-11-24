# Backups and virtual machines (src/backup/engine.c, vmmeta.h; docs/VIRTUALIZATION.md 6.14): temporary files of a
# "*.lordvm" left out, "backup": false, the shared flock held while a machine is copied, a running machine's disks
# carried from the previous snapshot by hard link, links never followed for lock or machine.json; under ASan and UBSan.
# backupvm-warn compiles the changed backup files with lord-server's warnings (without building lord-server).
test: build/backup-machines-test
build/backup-machines-test: tests/backup_machines_test.c src/backup/engine.c src/backup/crypt.c src/fsops.c src/sec/json.c \
		src/backup/engine.h src/backup/vmmeta.h src/backup/crypt.h src/fsops.h src/sec/json.h build/obj/vault/monocypher.o
	@mkdir -p build
	$(CC) $(CPPFLAGS) -isystem ../third_party/monocypher $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=c11 \
		-o $@ tests/backup_machines_test.c src/backup/engine.c src/backup/crypt.c src/fsops.c src/sec/json.c build/obj/vault/monocypher.o -lpthread
	./$@ || { rm -f $@; exit 1; }

.PHONY: backupvm-warn
backupvm-warn:
	$(CC) $(CPPFLAGS) -isystem ../third_party/monocypher $(CFLAGS) $(WARN) -Werror -std=c11 -c -o /dev/null src/backup/engine.c
	$(CC) $(CPPFLAGS) -isystem ../third_party/monocypher $(CFLAGS) $(WARN) -Werror -std=c11 -c -o /dev/null src/backup/backup.c
