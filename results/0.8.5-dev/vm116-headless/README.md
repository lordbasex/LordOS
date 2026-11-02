# vm116-headless — LordOS 0.8.5-dev

Collected 2026-10-26T19:34Z with `tools/results/collect.sh vm116-headless` (docs/RESULTS.md); the report's
own version field says `0.8.4.1-beta`.

**Conditions:** Proxmox VM 116 (x86-64, no GPU passed through, virtual display only), a development
install of the 0.8.5 tree in `/usr/local/lib/lordos` (so the report's version field still says the
packaged 0.8.4.1). The Test Bench ran in an isolated headless session (its own home and D-Bus) beside the
maintainer's session, so no memory figures were taken (`--no-memory`): the whole-system number would count
two sessions. Without a monitor the compositor presents no frame (every frame is discarded) and frame
callbacks come at about 1 Hz: the frame times here measure that, not drawing speed.

Reproduce:

```sh
lordos-sysreport report --json > sysreport.json
lordos-testbench --gl --auto 10 > testbench.json      # inside a LordOS session
free -m                                                  # whole system
sudo sh -c 'for f in /proc/[0-9]*/smaps_rollup; do ...'  # PSS per process: see tools/results/collect.sh
systemd-analyze
```
