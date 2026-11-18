#!/bin/sh
# session-sample.sh — samples one user's lord-server session: memory and processor time, every few seconds,
# into a CSV that anyone can analyse (docs/PERFORMANCE.md).
#
#   tools/bench/session-sample.sh <user> <minutes> <out.csv> [interval-seconds]
#
# It waits for that user's session (`lord-server --headless … --user <user>`, never the login screen's), then
# reads /proc/<pid>/status (VmRSS, RssAnon, RssFile) and /proc/<pid>/stat (utime + stime, in clock ticks) and
# /proc/meminfo (MemAvailable). Run it as the session's user or as root. It only reads; it changes nothing.
# Header lines start with '#': the LordOS version, theme, displays, kernel, processor and session start.
set -eu
U=${1:?user}; MIN=${2:?minutes}; OUT=${3:?out.csv}; IV=${4:-10}
find_pid() {
    for p in $(pgrep -x lord-server 2>/dev/null); do
        a=$(tr '\0' ' ' < /proc/"$p"/cmdline 2>/dev/null) || continue
        case "$a" in *--greeter*) continue ;; *" --user $U "*) echo "$p" ;; esac
    done | tail -n 1
}
PID=$(find_pid)
while [ -z "$PID" ]; do sleep 5; PID=$(find_pid); done
HZ=$(getconf CLK_TCK)
HOME_U=$(getent passwd "$U" | cut -d: -f6)
theme=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('theme','default'))" "$HOME_U/.config/lordos/settings.json" 2>/dev/null || echo '?')
displays=$(for f in /sys/class/drm/card*-*/status; do [ "$(cat "$f")" = connected ] && basename "$(dirname "$f")"; done 2>/dev/null | tr '\n' ' ')
started=$(ps -o lstart= -p "$PID")
{
    echo "# lordos $(dpkg-query -W -f '${Version}' lordos 2>/dev/null)"
    echo "# theme $theme"
    echo "# displays ${displays:-none}"
    echo "# kernel $(uname -r) $(uname -m)"
    echo "# cpu $(grep -m1 -E 'model name|Model' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "# memory_total_kb $(awk '/^MemTotal/{print $2}' /proc/meminfo)"
    echo "# session_pid $PID started $started"
    echo "# clk_tck $HZ interval_s $IV"
    echo "t_s,rss_kb,anon_kb,file_kb,cpu_ticks,mem_available_kb"
} > "$OUT"
end=$(( $(date +%s) + MIN * 60 )); t0=$(date +%s)
while [ "$(date +%s)" -lt "$end" ]; do
    [ -d /proc/"$PID" ] || { echo "# session ended at $(( $(date +%s) - t0 )) s" >> "$OUT"; exit 1; }
    s=$(awk '/^VmRSS/{r=$2}/^RssAnon/{a=$2}/^RssFile/{f=$2}END{print r","a","f}' /proc/"$PID"/status)
    c=$(awk '{print $14+$15}' /proc/"$PID"/stat)
    m=$(awk '/^MemAvailable/{print $2}' /proc/meminfo)
    echo "$(( $(date +%s) - t0 )),$s,$c,$m" >> "$OUT"
    sleep "$IV"
done
echo "# done" >> "$OUT"
