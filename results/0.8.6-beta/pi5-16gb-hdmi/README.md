# pi5-16gb-hdmi — LordOS 0.8.6-beta

Collected 2026-10-30T12:27Z with `tools/results/collect.sh pi5-16gb-hdmi` (docs/RESULTS.md); the report's
own version field says `0.8.6-beta`.

**Conditions:** Raspberry Pi 5 16 GB, LordOS 0.8.6-beta release package updated from 0.8.5 with `apt`; HDMI monitor at
1920×1080 60 Hz; started over SSH about a minute after a restart, inside the autologin session; no apps opened
by hand; official power supply; `ondemand` governor.

Reproduce:

```sh
lordos-sysreport report --json > sysreport.json
lordos-testbench --gl --auto 30 > testbench.json      # inside a LordOS session
free -m                                                  # whole system
sudo sh -c 'for f in /proc/[0-9]*/smaps_rollup; do ...'  # PSS per process: see tools/results/collect.sh
systemd-analyze
lordos-sysreport bench --full --json > lordbench.json
```
