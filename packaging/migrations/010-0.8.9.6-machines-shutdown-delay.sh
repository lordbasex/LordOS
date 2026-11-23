# 0.8.9.6: a shutdown or restart of the computer waits up to 45 s (was 30 s) for running machines
# (/usr/lib/systemd/logind.conf.d/lordos-machines.conf, InhibitDelayMaxSec; docs/VIRTUALIZATION.md 6.1), so a guest
# that can't be saved to disk (a graphics card, 3D graphics) has time for its own ACPI shutdown. dpkg has replaced the
# drop-in already; logind reads it only when it starts or reloads, so without this the restart that follows the upgrade
# would still give the machines 30 s. lord-vm plans each stop from logind's live InhibitDelayMaxUSec either way.
# Reloading logind keeps the sessions and changes nothing else (an administrator's own value in /etc still wins).
# Does nothing without systemd (an image being built). Running it again only reloads logind again.
[ -d /run/systemd/system ] || exit 0
command -v systemctl >/dev/null 2>&1 || exit 0
systemctl reload systemd-logind 2>/dev/null || true
exit 0
