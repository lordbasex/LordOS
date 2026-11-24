#!/bin/sh
# ns-run.sh — runs a command the way the Machines harness runs lord-server (README.md here): in its own user and mount
# namespaces, with an empty folder over the real home, a tmpfs over the real runtime folder and a private HOME and
# XDG_RUNTIME_DIR in a scratch folder, as this account. For lord-vm, the release gates or any script that would otherwise
# see the person's files, machines or session.
#   sh tools/vm/e2e/machines/ns-run.sh [--keep] COMMAND [ARG ...]
# The scratch folder is /var/tmp/lordos-m100-e2e/ns.XXXXXX (MH_SCRATCH), removed afterwards unless --keep; its path is in
# MH_NS_DIR for the command. Exit status: the command's.
set -u
KEEP=0
[ "${1:-}" = --keep ] && { KEEP=1; shift; }
[ $# -gt 0 ] || { echo "usage: ns-run.sh [--keep] COMMAND [ARG ...]"; exit 2; }
command -v unshare >/dev/null 2>&1 && unshare -Urm true 2>/dev/null || { echo "skipped: needs unprivileged user namespaces (unshare)"; exit 77; }
S=${MH_SCRATCH:-/var/tmp/lordos-m100-e2e}
mkdir -p "$S" && D=$(mktemp -d "$S/ns.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home" "$D/run" "$D/fake"
chmod 700 "$D/home" "$D/run"
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u DBUS_SESSION_BUS_ADDRESS -u LORD_SESSIOND -u LORDOS_SESSIOND \
    HOME="$D/home" XDG_RUNTIME_DIR="$D/run" MH_NS_DIR="$D" LANG=C.UTF-8 MH_U="$(id -u)" MH_G="$(id -g)" \
    unshare -Urm sh -c 'mount --bind "$1" "$2" && { [ -d "$3" ] && mount -t tmpfs -o mode=700 tmpfs "$3" || true; } && shift 3 &&
        exec unshare --user --map-user="$MH_U" --map-group="$MH_G" "$@"' ns "$D/fake" "$REALHOME" "$RT" "$@"
rc=$?
[ $KEEP = 1 ] && echo "kept: $D" || rm -rf "$D"
exit $rc
