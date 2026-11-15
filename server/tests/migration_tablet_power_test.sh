#!/bin/sh
# migration_tablet_power_test.sh — packaging/migrations/008-0.8.8.9-tablet-power.sh over fake system
# trees (LORDOS_MIGRATION_ROOT): which machines count as tablets, which choices it keeps, and that a
# second run changes nothing. Run from server/ by tests/run_suites.sh.
set -u
M=$(cd "$(dirname "$0")/../.." && pwd)/packaging/migrations/008-0.8.8.9-tablet-power.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-mig008.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0 n=0
F=etc/systemd/logind.conf.d/lordos.conf
NEW='# Written by LordOS (Settings → Battery). Changes here are overwritten.
[Login]
HandlePowerKey=suspend
HandlePowerKeyLongPress=poweroff
HandleLidSwitch=suspend
HandleLidSwitchExternalPower=suspend'

# tree NAME CHASSIS: a fresh system tree with that chassis type ("" = no DMI) and Debian's logind.conf
tree() {
    R="$T/$1"
    mkdir -p "$R/sys/class/dmi/id" "$R/etc/systemd"
    [ -z "$2" ] || printf '%s\n' "$2" > "$R/sys/class/dmi/id/chassis_type"
    printf '[Login]\n#HandlePowerKey=poweroff\n#HandleLidSwitch=suspend\n' > "$R/etc/systemd/logind.conf"
}
put() { mkdir -p "$(dirname "$R/$1")"; printf '%s\n' "$2" > "$R/$1"; }
# migrate: runs it twice (the second run must change nothing)
migrate() {
    LORDOS_MIGRATION_ROOT=$R sh -eu "$M" || { echo "FAIL $name: the migration exited with $?"; fails=$((fails + 1)); return; }
    [ -e "$R/$F" ] && cp "$R/$F" "$T/first" || rm -f "$T/first"
    LORDOS_MIGRATION_ROOT=$R sh -eu "$M" || { echo "FAIL $name: the second run exited with $?"; fails=$((fails + 1)); return; }
    if [ -e "$T/first" ]; then cmp -s "$T/first" "$R/$F" || { echo "FAIL $name: the second run changed lordos.conf"; fails=$((fails + 1)); }
    elif [ -e "$R/$F" ]; then echo "FAIL $name: the second run wrote lordos.conf"; fails=$((fails + 1)); fi
}
# expect TEXT (or "none"): lordos.conf afterwards
expect() {
    n=$((n + 1))
    if [ "$1" = none ]; then
        [ ! -e "$R/$F" ] && return
        echo "FAIL $name: lordos.conf written"; fails=$((fails + 1)); return
    fi
    got=$(cat "$R/$F" 2>/dev/null)
    [ "$got" = "$1" ] && return
    echo "FAIL $name: lordos.conf is"; printf '%s\n' "$got" | sed 's/^/  | /'; fails=$((fails + 1))
}

name="tablet (30), nothing chosen"; tree a 30; migrate; expect "$NEW"
name="detachable (32), nothing chosen"; tree b 32; migrate; expect "$NEW"
for c in 31 9 10 3 1 '' junk ' 30' 300; do
    name="chassis '$c' is not a tablet"; tree "c$c" "$c"; migrate; expect none
done
name="no logind.conf at all"; tree d 30; rm "$R/etc/systemd/logind.conf"; migrate; expect "$NEW"

name="an older Settings' file with the old default: power changes, the lid stays"
tree e 30; put "$F" '# Written by LordOS (Settings → Battery). Changes here are overwritten.
[Login]
HandlePowerKey=poweroff
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=hibernate'
migrate; expect '# Written by LordOS (Settings → Battery). Changes here are overwritten.
[Login]
HandlePowerKey=suspend
HandlePowerKeyLongPress=poweroff
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=hibernate'

OWN='# Written by LordOS (Settings → Battery). Changes here are overwritten.
[Login]
HandlePowerKey=hibernate
HandleLidSwitch=suspend
HandleLidSwitchExternalPower=suspend'
name="the person chose another short press"; tree f 30; put "$F" "$OWN"; migrate; expect "$OWN"

HOLD='# Written by LordOS (Settings → Battery). Changes here are overwritten.
[Login]
HandlePowerKey=poweroff
HandlePowerKeyLongPress=ignore
HandleLidSwitch=suspend
HandleLidSwitchExternalPower=suspend'
name="the person chose with the hold row (even shut down on a tap)"; tree g 30; put "$F" "$HOLD"; migrate; expect "$HOLD"

name="an administrator's HandlePowerKey in logind.conf"; tree h 30
printf '[Login]\nHandlePowerKey=poweroff\n' > "$R/etc/systemd/logind.conf"; migrate; expect none
name="an administrator's drop-in"; tree i 30; put etc/systemd/logind.conf.d/50-site.conf '[Login]
HandlePowerKeyLongPress=reboot'; migrate; expect none

name="lid values from logind.conf are kept"; tree j 30
printf '[Login]\nHandleLidSwitch=ignore\n' > "$R/etc/systemd/logind.conf"; migrate
expect '# Written by LordOS (Settings → Battery). Changes here are overwritten.
[Login]
HandlePowerKey=suspend
HandlePowerKeyLongPress=poweroff
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=ignore'
name="a lid value Settings can't show: left alone"; tree k 30
printf '[Login]\nHandleLidSwitch=lock\n' > "$R/etc/systemd/logind.conf"; migrate; expect none

name="a link where lordos.conf goes is never followed"; tree l 30; mkdir -p "$R/etc/systemd/logind.conf.d"
ln -s "$T/away" "$R/$F"; migrate; n=$((n + 1))
if [ -e "$T/away" ] || [ ! -L "$R/$F" ]; then echo "FAIL $name"; fails=$((fails + 1)); fi

echo "migration 008 (tablet power): $((n - fails))/$n passed"
[ "$fails" = 0 ]
