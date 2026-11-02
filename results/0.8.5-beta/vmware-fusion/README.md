# vmware-fusion — LordOS 0.8.5-beta

Collected 2026-10-27T14:26Z with `tools/results/collect.sh vmware-fusion` (docs/RESULTS.md); the report's
own version field says `0.8.5-beta`.

**Conditions:** a VMware Fusion virtual machine on the maintainer's Mac (Intel Core i9-10900, 4 vCPU,
4.7 GB RAM), its virtual display at 1280×800 and 60 Hz, GPU composition on the SVGA3D driver; automatic
login. Updated from 0.8.4.1 to this 0.8.5 package with LordOS's package engine against
lordos.org/apt-staging, then restarted; measured about five minutes after that restart, with
whatever windows the session opened at login and no app started by hand (Chrome, VS Code, Spotify and llama.cpp
installed, not running). Mains power, no profile (a VM).

Reproduce:

```sh
lordos-sysreport report --json > sysreport.json
lordos-testbench --gl --auto 30 > testbench.json      # inside a LordOS session
free -m                                                  # whole system
sudo sh -c 'for f in /proc/[0-9]*/smaps_rollup; do ...'  # PSS per process: see tools/results/collect.sh
systemd-analyze
```
