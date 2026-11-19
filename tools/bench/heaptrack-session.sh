#!/bin/sh
# heaptrack-session.sh — runs ONE user's next lord-server session under heaptrack, to see which code holds
# the session's heap memory (docs/perf/MEMORY-HEAPTRACK.md). For test machines only; run as root.
#
#   tools/bench/heaptrack-session.sh install <user> <lord-server binary with symbols> [out-dir]
#   tools/bench/heaptrack-session.sh restore
#
# install: moves nothing away. It copies the given binary (build/lord-server, NOT stripped: heaptrack needs
# its symbols to name the lines) to /usr/lib/lordos/bin/lord-server.real, keeps the installed binary as
# lord-server.orig, and puts a small sh wrapper in its place. lord-sessiond starts every lord-server through
# that path; the wrapper starts heaptrack only for the first session of <user> (`--headless … --user <user>`)
# and runs the binary directly for everything else (the login screen, other users, a second login). If the
# session under heaptrack doesn't come up, logging in again gives a normal one.
#
# restore: puts lord-server.orig back and removes the wrapper and lord-server.real.
#
# Notes that cost us a test run each (0.8.9.1):
#  - lord-sessiond starts lord-server with an empty environment: no PATH. The wrapper sets its own, or heaptrack
#    (a sh script) and `id` are not found and the wrapper silently runs the plain binary.
#  - the helpers lord-server starts re-execute /proc/self/exe, which is lord-server.real: they don't go through
#    the wrapper, and heaptrack's preload library clears LD_PRELOAD so child programs are not traced.
set -eu
BIN=${LORDOS_BIN:-/usr/lib/lordos/bin}     # (LORDOS_BIN: only to test this script)
case "${1:-}" in
install)
    U=${2:?user}; SRC=${3:?lord-server binary with symbols}; OUT=${4:-/var/tmp/lordos-heaptrack}
    command -v heaptrack >/dev/null || { echo "heaptrack is not installed (apt install heaptrack)" >&2; exit 1; }
    file "$SRC" | grep -q 'not stripped' || echo "warning: $SRC is stripped; heaptrack will show addresses, not lines" >&2
    case "$U" in *[!a-z0-9_-]*|'') echo "bad user name" >&2; exit 1 ;; esac
    install -d -o "$U" -m 755 "$OUT"
    rm -rf "$OUT/used"
    [ -e "$BIN/lord-server.orig" ] || cp -p "$BIN/lord-server" "$BIN/lord-server.orig"
    install -m 755 "$SRC" "$BIN/lord-server.real"
    cat > "$BIN/lord-server.new" <<EOF
#!/bin/sh
# heaptrack wrapper (tools/bench/heaptrack-session.sh): the next session of $U runs under heaptrack, once
export PATH=/usr/local/bin:/usr/bin:/bin
REAL=$BIN/lord-server.real
OUT=$OUT
case " \$* " in
*" --headless "*" --user $U "*)
    if mkdir "\$OUT/used" 2>/dev/null; then
        exec /usr/bin/heaptrack -o "\$OUT/heaptrack-lord-server" "\$REAL" "\$@"
    fi ;;
esac
exec "\$REAL" "\$@"
EOF
    chmod 755 "$BIN/lord-server.new"
    mv "$BIN/lord-server.new" "$BIN/lord-server"
    echo "ready: log $U out and in again; the trace goes to $OUT/heaptrack-lord-server.*"
    ;;
restore)
    [ -e "$BIN/lord-server.orig" ] || { echo "no $BIN/lord-server.orig to restore" >&2; exit 1; }
    mv "$BIN/lord-server.orig" "$BIN/lord-server"
    rm -f "$BIN/lord-server.real"
    echo "restored: the running session keeps its binary until the next login"
    ;;
*)
    sed -n '2,9p' "$0" >&2; exit 2 ;;
esac
