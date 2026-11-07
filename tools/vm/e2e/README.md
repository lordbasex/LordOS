# Machines end-to-end bench

A real headless LordOS session on the amd64 build VM (VM 116), built from this branch, driving the New Machine
assistant's download cards on screen: a card chosen, its maker's signed list read, the image downloaded through the app
(progress, Stop Download, resumed), the machine created and started, a whole Debian install to its disk, the reboot into
the installed system and a login. The approach is the Cloud bench's (branch `cloud-e2e`, `tools/cloud/e2e`): a test-only
theme bundle with a driver, commands run in the session as its user, input and pictures over the session's
`remote.sock`. Test only: nothing here ships, and the accounts are throwaway ones.

| File | What |
|---|---|
| `session.sh` | as root: `install TREE` (to `/opt/lordos-vm-e2e`, root-owned, and the catalog with the makers' keys in `/usr/lib/lordos/assets/machines`), `start USER` (lord-server as root with `--user`, as lord-sessiond does, so the session has its administration helper), `stop`, `status`, `purge` |
| `driver.js`, `build-e2e.mjs` | the bundle: the real default theme plus a driver that runs `~/.e2e/cmd-N.js` with `lord`, `wm`, `admin`, `launchApp` |
| `lc.py` | `lc.py USER JS [TIMEOUT]` (as root): runs JS in the session |
| `ui.py` | `ui.py SOCK cmd…` (as root): clicks, keys, typing with the whole US keyboard (the VM window passes key codes to the guest), `grab:FILE.png` |
| `preseed-debian.cfg` | the answers of an unattended Debian 13 install (user `tester`, password `Vm-guest-0001`) |

## Run

```sh
node tools/build-theme.mjs && node tools/vm/e2e/build-e2e.mjs                    # on the Mac
rsync -a --delete --exclude .git --exclude node_modules --exclude server/build --exclude /out ./ VM:LordOS-089/
ssh VM 'cd ~/LordOS-089/server && make -j6 && sudo sh ../tools/vm/e2e/session.sh install ~/LordOS-089'
ssh VM 'sudo sh ~/LordOS-089/tools/vm/e2e/session.sh start m89a'
ssh VM 'sudo python3 -I ~/LordOS-089/tools/vm/e2e/lc.py m89a "launchApp(\"machines\"); await sleep(2000); return wm.windows()"'
ssh VM 'sudo python3 -I ~/LordOS-089/tools/vm/e2e/ui.py /run/vm-e2e-rt/m89a/lordos/remote.sock grab:/tmp/a.png'
```

The Debian install: serve `preseed-debian.cfg` from another computer on the build VM's network
(`python3 -m http.server 8089 --bind <its address>` in a folder with only that file), and at the installer's boot menu
press Down (Install), `e`, Down three times to the `linux` line, End, type ` auto=true priority=critical
url=http://<its address>:8089/preseed-debian.cfg`, then Ctrl+X. Not from the build VM itself: with passt the guest has
the build VM's own address, and the Shared network doesn't map the host's loopback to the gateway (`--no-map-gw`), so
the host isn't reachable from its guests (by design, 6.5). In `ui.py`'s arguments put a typed text with spaces or `;`
in single quotes inside the ssh command.
