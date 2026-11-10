#!/bin/sh
# screencast_test.sh BUILD [bench] — lord-screencast against a private PipeWire daemon of its own (its own runtime
# folder, no session manager, nothing of the session's), driven by BUILD/screencast-test. Without pipewire it says
# "skipped: needs pipewire" and passes.
B=${1:-build}
shift
if ! command -v pipewire >/dev/null 2>&1; then echo "screencast-test: skipped: needs pipewire"; exit 0; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-cast.XXXXXX") || exit 1
chmod 700 "$T"
PW=
trap '[ -n "$PW" ] && kill "$PW" 2>/dev/null; rm -rf "$T"' EXIT
cat >"$T/daemon.conf" <<'EOF'
context.properties = {
    core.daemon = true
    core.name = pipewire-0
    support.dbus = false
    log.level = 1
}
context.spa-libs = {
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-access }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
]
EOF
XDG_RUNTIME_DIR="$T" PIPEWIRE_RUNTIME_DIR="$T" pipewire -c "$T/daemon.conf" >"$T/daemon.log" 2>&1 &
PW=$!
i=0
while [ ! -S "$T/pipewire-0" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
if [ ! -S "$T/pipewire-0" ]; then cat "$T/daemon.log"; echo "screencast-test: the private daemon didn't start"; exit 1; fi
XDG_RUNTIME_DIR="$T" PIPEWIRE_RUNTIME_DIR="$T" timeout 60 "$B/screencast-test" "$B/lord-screencast" "$T/pipewire-0" "$@"
r=$?
[ $r -ne 0 ] && tail -n 20 "$T/daemon.log"
exit $r
