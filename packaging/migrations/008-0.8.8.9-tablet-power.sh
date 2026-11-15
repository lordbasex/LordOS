# 0.8.8.9: on a tablet a tap on the power button sleeps and a 5 s hold shuts down (issue #131,
# docs/SETTINGS.md "Power button and lid"); before, a tap shut it down. A tablet is SMBIOS chassis type
# 30 (Tablet) or 32 (Detachable) in /sys/class/dmi/id/chassis_type, the rule of
# server/src/hw/tabletpower.h; 31 (Convertible) keeps a laptop's defaults. New installs get it from the
# disk installer (this migration runs while the live image is built, in a chroot without systemd, and
# does nothing there).
#
# Only where the person hasn't chosen: /etc/systemd/logind.conf.d/lordos.conf missing, or written by
# an older Settings with the short press on poweroff (the old default) and no hold row
# (HandlePowerKeyLongPress, which every Settings since #131 writes). Its lid values are kept. Nothing
# is done when logind.conf or another drop-in of /etc sets HandlePowerKey or HandlePowerKeyLongPress
# (an administrator's own setting) or a lid value Settings can't show. A second run finds the hold
# line and stops. LORDOS_MIGRATION_ROOT is for tests only (server/tests/migration_tablet_power_test.sh).
R=${LORDOS_MIGRATION_ROOT:-}
if [ -z "$R" ]; then [ -d /run/systemd/system ] || exit 0; fi
t=$(cat "$R/sys/class/dmi/id/chassis_type" 2>/dev/null || true)
case "$t" in 30|32) ;; *) exit 0 ;; esac
C="$R/etc/systemd/logind.conf" D="$R/etc/systemd/logind.conf.d"
F="$D/lordos.conf"
for f in "$C" "$D"/*.conf; do
    [ -f "$f" ] && [ "$f" != "$F" ] || continue
    if grep -Eq '^HandlePowerKey(LongPress)?=' "$f"; then exit 0; fi
done
# the last value a file gives a key (empty: unset)
val() { sed -n "s/^$1=\([^[:space:]]*\).*/\1/p" "$2" | tail -n 1; }
if [ -e "$F" ] || [ -L "$F" ]; then
    [ -f "$F" ] && [ ! -L "$F" ] || exit 0
    if grep -q '^HandlePowerKeyLongPress=' "$F"; then exit 0; fi
    p=$(val HandlePowerKey "$F")
    [ -z "$p" ] || [ "$p" = poweroff ] || exit 0
fi
lid=suspend lidac=
for f in "$C" "$F"; do
    [ -f "$f" ] || continue
    v=$(val HandleLidSwitch "$f"); [ -z "$v" ] || lid=$v
    v=$(val HandleLidSwitchExternalPower "$f"); [ -z "$v" ] || lidac=$v
done
[ -n "$lidac" ] || lidac=$lid
for v in "$lid" "$lidac"; do
    case "$v" in poweroff|suspend|hibernate|ignore) ;; *) exit 0 ;; esac
done
mkdir -p "$D"
printf '# Written by LordOS (Settings → Battery). Changes here are overwritten.\n[Login]\nHandlePowerKey=suspend\nHandlePowerKeyLongPress=poweroff\nHandleLidSwitch=%s\nHandleLidSwitchExternalPower=%s\n' \
    "$lid" "$lidac" > "$D/.lordos.conf.tmp"
chmod 644 "$D/.lordos.conf.tmp"
mv -f "$D/.lordos.conf.tmp" "$F"
if [ -z "$R" ] && command -v systemctl >/dev/null 2>&1; then
    systemctl reload systemd-logind 2>/dev/null || true
fi
exit 0
