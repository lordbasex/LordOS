# 0.8.9.6: shared memory gets transparent huge pages where it asks for them (shmem_enabled "advise";
# docs/VIRTUALIZATION.md 3.7.2), so a machine with a shared folder or the transfer folder (its memory is shared with
# virtiofsd) gets 2 MB pages like one without. The package's /usr/lib/tmpfiles.d/lordos-thp.conf sets it at every
# boot; this applies it now, so the upgrade doesn't wait for a restart. Only from "never" (the kernel's default): a
# value an administrator chose (always, within_size, deny, force, or advise already) stays. Only this one file.
# Does nothing without systemd (an image being built: /sys there may be the build host's) or without transparent
# huge pages (the file isn't there). Running it again changes nothing. LORDOS_MIGRATION_ROOT is for tests only
# (server/tests/migration_shmem_huge_test.sh).
R=${LORDOS_MIGRATION_ROOT:-}
if [ -z "$R" ]; then [ -d /run/systemd/system ] || exit 0; fi
F="$R/sys/kernel/mm/transparent_hugepage/shmem_enabled"
[ -f "$F" ] || exit 0
grep -q '\[never\]' "$F" 2>/dev/null || exit 0
echo advise > "$F" 2>/dev/null || true
exit 0
