# 0.8.8.8: LordOS's logs move to /var/log/lordos (server/src/lordlog.h, docs/SYSTEM.md "Log files"):
# the root daemons' files there (lord-sessiond, lord-netd, lord-fand, admin) and one 0700 folder per
# user under users/, which lord-sessiond makes at each login. Both made here too, with their modes:
# /var/log/lordos 0755 and users/ 0711 (anyone can reach their own folder, nobody can list the names).
# Only root can create anything in /var/log, so nothing found there was planted by a user; something
# that is not a directory is moved aside, never followed. The old ~/.cache/lordos/*.log are left where
# they are (a migration never touches home folders): the Logs app shows them under "Older logs".
# LORDOS_MIGRATION_ROOT is for tests only.
R=${LORDOS_MIGRATION_ROOT:-}
L="$R/var/log/lordos"
[ -d "$R/var/log" ] || mkdir -p "$R/var/log"
for d in "$L:0755" "$L/users:0711"; do
    p=${d%:*} m=${d##*:}
    if [ -L "$p" ] || { [ -e "$p" ] && [ ! -d "$p" ]; }; then mv -f "$p" "$p.not-a-folder"; fi
    [ -d "$p" ] || mkdir "$p"
    if [ "$(id -u)" = 0 ]; then chown root:root "$p"; fi
    chmod "$m" "$p"
done
exit 0
