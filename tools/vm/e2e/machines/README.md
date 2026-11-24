# Machines end-to-end harness

A real headless LordOS session that tests the Machines app the way a person uses it, on the computer you work on,
without touching your files, your session or your machines. Test only: nothing here ships.

- **The session.** `server/build/lord-server-test --headless 1280x800 --test-hooks` with the real default theme (plus the
  test-only `driver.js` and `probe.js`, bundled by `../build-e2e.mjs --out` into the run's own folder; a development
  build loads a theme outside the LordOS folder unchecked, so nothing in the tree is replaced), on the CPU path
  (`LORD_GPU=0`, `LORD_HEADLESS_DRAW=1`), US keyboard, English.
- **Private namespaces.** lord-server and everything it starts (lord-vm, QEMU, passt, virtiofsd) run in their own user
  and mount namespaces: root of the first only to mount an empty folder over your real home and a tmpfs over your
  runtime folder, then your account again. `HOME` and `XDG_RUNTIME_DIR` are folders of the run, under
  `/var/tmp/lordos-m100-e2e/` (`MH_SCRATCH`); its Machines library is `HOME/Machines`, empty at the start. The pattern of
  `server/tests/vm_group_link.sh`. `ns-run.sh COMMAND…` runs any other command the same way (lord-vm, the release gates).
- **Input.** Pointer, buttons, wheel and keys through the test hooks (`server/src/testhooks.h`: `ptr`, `btn`, `wheel`,
  `key`), which call the remote desktop's own injection (`main.c` `remote_pointer`, `remote_button`, `remote_wheel`,
  `remote_key`). `remote.sock` itself takes only root's connections (lord-sessiond's), so a run as your account uses the
  hooks; a run as root uses `remote.sock` (`RemoteInput`).
- **What is on screen.** `probe.js` mirrors lord-server's node tree on the JS side (it wraps the `lord.*` node calls before
  the runtime makes its first node): a test finds a text or an icon (by its picture's source), where it is drawn, whether
  it is shown and on top (painted last at its centre: not under a sheet, a menu or a question), and clicks it there.
  Pictures come from the hooks' `frame output` (exactly what the headless display shows), saved as PNG.
- **Commands in the session.** `driver.js` runs `~/.e2e/cmd-N.js` as the body of an async function with `lord`, `wm`
  (also `resize`, `setFrame`), `admin`, `launchApp`, `sleep`, `log`, `mh` (the probe) and `sdk` (`@lordos/sdk`, the same
  instances the apps use), and writes `out-N.json`.
- **Guests.** `images.py` keeps, in `/var/tmp/lordos-m100-images` (`MH_IMAGES`): Debian 13's nocloud image (SHA-512
  checked against Debian's list, fetched over HTTPS) and, made from it once under KVM, `images.agent_path()`
  (`debian13-agent2-<arch>.qcow2`) with qemu-guest-agent, a person's account (`tester`, uid 1000: dropped files go to a
  person's Downloads), root logged in on the serial console and `MH-READY` written there at each boot. Read only; the
  session gets a hard link in `~/Images` and Machines copies it in. `images.py --windows` adds Microsoft's Windows 11
  Enterprise Evaluation and virtio-win, each checked against the SHA-256 the catalog pins.
- **The guest tools** a machine installs (Install Clipboard Helper, the Windows tools) come from lord-vm's fixed paths,
  `/usr/lib/lordos/guest/`: the installed LordOS's, not this build's (a security choice of lord-vm). On a computer without
  LordOS installed those steps fail.

## Run

```sh
make -C server -j8 && npm run build                       # lord-server-test, lord-vm; node_modules for the bundle
make -C server e2e-machines                               # the image (once, ~2 min with the network), then every flow
make -C server e2e-machines E2E_FLOWS="library assistant" # some flows
make -C server e2e-machines-matrix [E2E_ROWS=kvm-uefi-2d] # the boot matrix
make -C server e2e-machines-gates [GATES_CYCLES=20]       # the three release gates, in ns-run.sh (~10 min)
python3 -I tools/vm/e2e/machines/run.py --keep --out /tmp/shots --json /tmp/run.json premade snapshots   # by hand
python3 -I tools/vm/e2e/machines/images.py --windows && python3 -I tools/vm/e2e/machines/run.py windows # ~9 GB, ~40 min
python3 -I tools/vm/e2e/machines/mh.py reap               # what a killed run left (its machines, its lord-server)
```

Each target prints `skipped: <why>` and exits 0 without KVM, the image (no network), `unshare`, node or the build. Exit
1 is a failure. `--keep` (`MH_KEEP=1`) leaves the run's folder; `--out DIR` (`MH_OUT`) copies its screenshots; `--json
FILE` writes the counts and the figures the flows measured (boot time, what deleting the clone freed…).

## The flows (`flows.py`)

One session and one library, in this order; a flow that needs what an earlier one made skips when it isn't there.

| Flow | What it does and checks |
|---|---|
| `library` | the Control Center, empty: its texts, its picture |
| `assistant` | New Machine → Import a machine → the file picker → `~/Images/debian13.qcow2` → Light → a typed name → Review → Create and Start; machine.json, the machine's window, the guest's boot (`MH-READY`) and its console's text in the window |
| `lifecycle` | Actions → Shut Down: the power button, the guest's own shutdown, Off |
| `configure` | the card's menu → Configure… (a sheet): Network → Isolated, Done; Guest agent on, Done; Shared, Done; each in machine.json |
| `start` | a double click on the card starts it in its window; it boots again |
| `agent` | qemu-guest-agent connects (`agentState`); the agent popover and what it says works; Install Clipboard Helper through it; lordos-guest in the guest |
| `filedrop` | a file dragged from Files onto the machine's window: the drop is taken, the file is in `~tester/Downloads` with the same bytes |
| `kills` | agent kill tests: qemu-guest-agent, passt and the transfer folder's virtiofsd killed mid-run, then lord-vm itself; each reported (agentState, the network back, the share-lost notice, supervisor-lost) and recovered |
| `snapshots` | Snapshots…: Take, listed by lord-vm, Go Back to This (a snapshot of now first), Delete |
| `clone` | Clone or Duplicate… → Clone: a second card with its own id and MAC |
| `storage` | what `lord.vm.storage()` (the Storage window) says deleting the clone frees, against the free space it gave back once in the Trash and the Trash emptied, on this run's file system |
| `group` | a machine group made by `lord-vm-testpaths --group-create` from two empty disks: its card in the library |

Not in the default list: `premade` (the assistant's machine made by lord-vm directly, so a later flow runs alone:
`run.py premade snapshots`) and `windows` (below).

## Using it from another test

```python
import sys; sys.path.insert(0, "tools/vm/e2e/machines")
from mh import Session, Results, main_guard, skip, kvm_ok
import images

def main():
    r = Results("my-feature")
    with Session(files={"Images/debian13.qcow2": images.agent_path()}) as s:
        s.js("launchApp('machines')")
        w = s.window_rect("machines")                 # [x, y, w, h] of the Control Center
        s.click_text("New Machine", within=w)         # a real click where the text is drawn (nth=-1: the last match)
        s.wait_text("What do you want to run?", 10)
        img = s.shot("assistant")                     # Image: .varied(r), .ink(r), .colors(r), .mean(r), .diff(other)
        r.t("the assistant is drawn", img.varied(w))
        s.click_icon("agent", within=w)               # an icon button: assets/machines/symbols/agent-symbolic.svg
        m = s.vm("MH Debian", timeout=60)             # lord.vm.list()'s entry; s.wait_state(name, "running", 90)
        con = s.console(m["id"])                      # the guest's serial console (root): out, status = con.run("uname -a")
        s.lordvm("--status", m["dir"], "--json")      # lord-vm in the session's namespaces
        r.t("no JS error", not s.errors())
    return r.end()                                    # 0 passed, 1 failed, 77 all skipped

main_guard(main)
```

Or add a function to `flows.py` (`flow_<name>(s, r, c)`, appended to `FLOWS` or `EXTRA`) and run it with `run.py <name>`;
it gets the shared `c` (`c["dir"]`, `c["id"]` of the assistant's machine; `flows.lib_rect(s)` opens and sizes the
Control Center, `flows.card_menu(s, name, entry)` opens a card's menu, `flows.allow_shortcuts(s)` answers the keyboard
shortcuts question the first machine window asks). From a shell script: `mh.py start [HOME_PATH=FILE …]` prints the run's
folder, then `mh.py js 'return wm.windows()'`, `mh.py ui text:'New Machine' type:abc key:ctrl+a click:100,200 wait:1`,
`mh.py shot NAME`, `mh.py stop`.

What a flow should check: the texts a person reads (by `wait_text`, never by coordinates of a mock-up), what landed on
disk or in lord-vm (`machine.json`, `lord-vm --status`, `log/lord-vm.log`, `log/serial.log`, which starts over at each
start), a picture that isn't a flat fill where something must be drawn (`ink` for a guest's console: two colours), and
`s.errors()` at the end. Keep a screenshot of each step (`s.shot`). Don't press Esc to close something over a machine's
window: the key goes to the guest; click elsewhere.

## The boot matrix (`boot-matrix.py`)

The prepared guest booted by lord-vm under each setting this computer can run, each in a scratch library: KVM and TCG,
OVMF/AAVMF and SeaBIOS, a TPM, Secure Boot with Microsoft's keys and a TPM, the compatible devices, 3D on each render
node, and the other architecture emulated (arm64 on amd64 needs `qemu-system-aarch64`; amd64 on the Pi needs
`qemu-system-x86_64` and the amd64 image there). Per row: seconds to `MH-READY`, to the agent's connection, a clean Shut
Down and its seconds, the disk's size; a failed row keeps the last lines its guest wrote. A row is skipped with less than
2.2 GB free. `--json FILE` and `--md FILE` write the results for docs/VIRTUALIZATION.md 12.

## The Windows (virtio-win) install test (`run.py windows`)

`images.py --windows` downloads Microsoft's Windows 11 Enterprise Evaluation (English) from Microsoft's own server and
the virtio-win drivers' disc from the project's folder, the URLs and SHA-256 the catalog pins (assets/machines/
systems.json, as the assistant's evaluation card fetches them), and keeps each only when its SHA-256 matches. The flow
gives both to `lord.vm.create` as the assistant does (Secure Boot, a TPM, Hyper-V, the windows devices, Install without
questions), starts the machine from its card, and waits for lord-vm's "Windows is installed" and the LordOS tools for
Windows saying hello from the installed system; a screenshot every five minutes of the install. About 40 minutes with
KVM. The admin helper isn't there in the harness (no root), so the assistant's own download step isn't what runs.

## Limits

- Apps mode and the clipboard need a guest with a desktop (apps mode a LordOS guest): the console-only Debian image
  can't show them; their guest side is `tests/guestwin_headless.sh` and `tests/guest_clip_test.sh`.
- Pictures are from the CPU path. A 3D machine's picture (dmabuf) needs `Session(gpu=True)` on a computer with a render
  node and isn't in the default flows. Machines' symbols (`svg:` images, drawn by lord-media) come out blank in the
  harness; `click_icon` finds them by their source, not their pixels.
- The session's screen is 1280×800 and its language English; flows find texts in English (`Session(lang=…)` exists, the
  flows' strings don't follow it).
- The Control Center is made 1200×680 before its sheets: at its first size (760×500) the Clone sheet's buttons are cut
  off below the window.
