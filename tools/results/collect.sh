#!/bin/sh
# collect.sh — the evidence for one machine and one LordOS version, written to results/<version>/<machine>/
# (docs/RESULTS.md). Run it as the user, inside a LordOS session (the Test Bench is a Wayland client):
#
#   tools/results/collect.sh <machine> [--no-memory] [--no-lordbench] [--bench-seconds N] [--version V]
#
# <machine> is a short name, [a-z0-9-] (e.g. vm116-headless, pi5-8gb-hdmi). It writes:
#   sysreport.json   lordos-sysreport report --json (no serial numbers, addresses or names)
#   testbench.json   lordos-testbench --gl --auto N (the Wayland checklist, frame times, 3D renderer)
#   memory.txt       whole system (`free -m`) and per process (PSS from /proc/<pid>/smaps_rollup); with
#                    sudo, root's processes too
#   boot.txt         systemd-analyze, and the time to the desktop's first frame from the kernel's start
#   boot-times.jsonl every recorded boot on this machine: seconds to the first frame, version, kernel
#   lordbench.json   lordos-sysreport bench --full --json (LordBench, about 5 minutes; --no-lordbench skips it)
#   README.md        when, what version, the conditions to fill in, and these exact commands
# The version is the installed one (software.lordos in the report); --version names it for a development
# build, e.g. 0.8.5-dev.
# Nothing is sent anywhere.
set -eu
M=${1:-}
case "$M" in ''|*[!a-z0-9-]*) echo "usage: $0 <machine [a-z0-9-]> [--no-memory] [--no-lordbench] [--bench-seconds N]" >&2; exit 2 ;; esac
shift
MEM=1 LB=1 SECS=30 V=
while [ $# -gt 0 ]; do
    case "$1" in
        --no-memory) MEM=0 ;;
        --no-lordbench) LB=0 ;;
        --bench-seconds) SECS=$2; shift ;;
        --version) V=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
case "$SECS" in ''|*[!0-9]*) echo "--bench-seconds takes a number" >&2; exit 2 ;; esac
command -v lordos-sysreport >/dev/null || { echo "lordos-sysreport not installed" >&2; exit 1; }
[ -n "$V" ] || V=$(lordos-sysreport report --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["software"]["lordos"])' 2>/dev/null || echo unknown)
case "$V" in *[!A-Za-z0-9.+~-]*|'') V=unknown ;; esac
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ -d "$ROOT/tools/results" ] || ROOT=$PWD      # copied on its own (not in the repository): write here
D="$ROOT/results/$V/$M"
mkdir -p "$D"

lordos-sysreport report --json > "$D/sysreport.json"

if [ -n "${WAYLAND_DISPLAY:-}" ] && command -v lordos-testbench >/dev/null; then
    timeout $((SECS + 30)) lordos-testbench --gl --auto "$SECS" > "$D/testbench.json" || echo "the Test Bench ended with an error" >&2
else
    echo "no Wayland session (WAYLAND_DISPLAY) or lordos-testbench missing: testbench.json skipped" >&2
fi

if [ "$MEM" = 1 ]; then
    {
        echo "# free -m (whole system; 'used' excludes the reclaimable page cache)"
        free -m
        echo
        echo "# PSS per process, MB (shared pages divided among the processes that map them)"
        echo "# $( [ "$(id -u)" = 0 ] && echo 'all processes' || echo 'only processes this user can read; run with sudo for all')"
        for f in /proc/[0-9]*/smaps_rollup; do
            p=${f%/smaps_rollup}
            k=$(awk '/^Pss:/ {print $2}' "$f" 2>/dev/null) || continue
            [ -n "$k" ] || continue
            printf '%s %s\n' "$k" "$(cat "$p/comm" 2>/dev/null)"
        done | sort -rn | awk '{ k = $1; $1 = ""; t += k; printf "%8.1f %s\n", k / 1024, $0 } END { printf "%8.1f  total of the processes listed\n", t / 1024 }'
    } > "$D/memory.txt"
fi

J() { if sudo -n true 2>/dev/null; then sudo -n journalctl "$@"; else journalctl "$@"; fi; }   # all entries if allowed
{
    systemd-analyze 2>&1 || true
    echo
    echo "# the desktop's first frame, from the kernel's start (lord-sessiond); the splash's own timer starts later"
    J -b -u lordos --no-pager 2>/dev/null | grep -o "first frame on the monitor [0-9.]* s after the kernel started" ||
        echo "(not in the journal: run with sudo, or as a member of the adm or systemd-journal group)"
    echo "# the boot splash started: subtract the first line's monotonic time from the second's"
    J -b -o short-monotonic --no-pager 2>/dev/null | awk '/kernel: Linux version/ && !k { print; k = 1 } /Started plymouth-start.service/ && !p { print; p = 1 }' |
        sed -E 's/^(\[[^]]*\]) [^ ]+ /\1 /' || true            # without the host name: results are published
} > "$D/boot.txt"
# every boot's time to the first frame on this machine, with its version (lord-sessiond; no names)
[ -r /var/lib/lordos/boot-times.jsonl ] && cp /var/lib/lordos/boot-times.jsonl "$D/boot-times.jsonl"

# last: its load would disturb the memory and frame times above
if [ "$LB" = 1 ]; then
    lordos-sysreport bench --full --json > "$D/lordbench.json" || { rm -f "$D/lordbench.json"; echo "LordBench ended with an error: lordbench.json skipped" >&2; }
fi

cat > "$D/README.md" <<MD
# $M — LordOS $V

Collected $(date -u +%Y-%m-%dT%H:%MZ) with \`tools/results/collect.sh $M\` (docs/RESULTS.md); the report's
own version field says \`$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["software"]["lordos"])' "$D/sysreport.json" 2>/dev/null)\`.

**Conditions** (fill in): hardware or VM and its resources; monitor (model, mode) or headless; how long
the system was idle before measuring; which apps were open; power source and profile.

Reproduce:

\`\`\`sh
lordos-sysreport report --json > sysreport.json
lordos-testbench --gl --auto $SECS > testbench.json      # inside a LordOS session
free -m                                                  # whole system
sudo sh -c 'for f in /proc/[0-9]*/smaps_rollup; do ...'  # PSS per process: see tools/results/collect.sh
systemd-analyze
lordos-sysreport bench --full --json > lordbench.json
\`\`\`
MD
echo "written to $D"
