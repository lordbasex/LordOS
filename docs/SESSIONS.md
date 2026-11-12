# Sessions, login screen and fast user switching (M8)

LordOS has its own session manager, `lord-sessiond`. It does the same as GDM or any other
"display manager":

- it shows the login screen on the monitor;
- it verifies passwords;
- it opens one session per user;
- it lets several sessions live at the same time (fast user switching);
- it also serves the remote desktop on the LAN (see [REMOTE.md](REMOTE.md)).

![Login screen](login-screen.png)

## Pieces

```
lordos.service (root)
└── lord-sessiond                     main: config, session list, passwords, monitor
    ├── lord-netd (netsup.c)          the system network, from boot (NETWORK.md); restarted if it exits
    ├── web connections (conn.c)      one child per browser; becomes the user (REMOTE.md)
    └── workers                       short-lived children: verify password, create the session
lord-server --greeter --user lordos-greeter   login screen (theme themes/greeter)
lord-server --user lord               one session per user, alive in the background
lord-server --user bob
```

### One session per user

Each session is a `lord-server --headless WxH --user <u>`. It starts as root and drops privileges
right away, as before. It has two states:

- **On the monitor.** It draws straight to the card, like a regular lord-server:
  - GPU composition (Mesa: EGL, GBM, GLES2) if there's a card with 3D;
  - linux-dmabuf for apps and games, and direct fullscreen scanout;
  - cursor via the card's plane;
  - without a GPU, the usual CPU path.

  The session doesn't open devices and isn't root: lord-sessiond **lends** it the card and the
  input (see below).
- **Off the monitor.** It draws in memory, on the CPU, and is only seen from the browser. If
  nobody is watching, it doesn't draw: it uses no CPU, but its programs, terminals and Linux apps
  keep running.

A locked session is still a session that draws for anyone watching `remote.sock`, which only root can
reach. The screen-sharing API keeps its own lock state for this reason ([SCREEN-SHARING.md](SCREEN-SHARING.md)).

**The same user is the same session.** Everyone connects to its socket
`$XDG_RUNTIME_DIR/lordos/remote.sock`: lord-sessiond, which lends it the monitor, and the
browsers, which watch it (REMOTE.md). That's why the monitor and the web show the same thing, and
both drive the mouse and keyboard. Different users have different sessions, which work in
parallel.

If a browser watches the session that's on the monitor, the session composites on the CPU for as
long as that connection lasts (the image has to be in memory to send it). When nobody is left
watching, it goes back to the GPU on its own. It was like that in M7 too.

- **Creation:** when creating a session, the monitor and the network take a per-user lock
  (`/run/lordos/spawn-<uid>.lock`). So two sessions of the same user are never created.
- **`/run/user/<uid>`:** if it doesn't exist, `user@<uid>.service` is started, which also starts
  its PipeWire. Since 0.8.5 `lord-sessiond` starts `user@<uid>` and waits for the user's session bus
  socket (up to 5 s) before starting the session: with automatic login the session used to start a
  few milliseconds before the bus existed, so `lord-vault` (the Secret Service) and the notification
  server never got on it (`PASSWORDS.md`).
- **GPU:**
  - by default, each session uses it while it's on the monitor;
  - with `lordos.gpu=0` on the kernel line (grub's safe mode) or `LORD_GPU=0` in the service
    (`systemctl edit lordos.service`, `Environment=LORD_GPU=0`), lord-sessiond passes `LORD_GPU=0` to
    all of them; `lordos.swcursor=1` or `LORD_SW_CURSOR=1` keeps the GPU but draws the pointer in the
    composition. A session already open keeps what it started with (the service's restart keeps it):
    log out first (`GPU.md`, "Where to set them");
  - `LORD_GPU=1` forces the GPU, even in software (llvmpipe).
- **Language:** it comes from `/etc/default/locale`. If the user has `~/.config/locale.conf`,
  that one is used, as before.

### The monitor: the card is lent (like logind's "session devices")

`server/src/remote/monitor.c`, in the main process (root):

1. **It opens the devices:**
   - the card, `/dev/dri/card0`, again for each session, and makes it DRM master;
   - keyboards, mice, tablets, touchpads and touchscreens, `/dev/input/event*` (touchpads are
     handled by the session's own driver, `server/src/tp`; see `TRACKPAD.md`).
2. **It passes them to the session** with `RP_DISPLAY`, over its socket (`SCM_RIGHTS`). The
   session prepares the output (`kms_attach`), takes the input (`input_open_fds` and
   `EVIOCGRAB`) and resizes live to the monitor's resolution:
   - the work area and the windows adjust;
   - Wayland apps receive the new mode.
3. **To switch sessions:**
   - it tells the leaving session (`RP_UNDISPLAY`), which goes back to drawing in memory;
   - it revokes its input with `EVIOCREVOKE`: its fds stop working even if it doesn't close them;
   - it takes master away with `DROP_MASTER` on its own copy of the same open file;
   - only then does it give a new card and input to the incoming session.

   Since 0.8.5 the revoked input fds are closed on a short-lived detached thread (`close_inputs` in
   `monitor.c`): the last close of a USB mouse stops its transfers, about 0.7 s for VMware's virtual
   USB mouse, and the hand-off used to wait for it. Access is still cut at once by `EVIOCREVOKE`.
   Measured on a VMware Fusion VM: 903 ms → 141 ms from the lock to the sign-in screen. A slow
   hand-off is logged with the time of each step.

**Hot plugging and unplugging:**

- **On plug:** `lord-sessiond` watches `/dev/input` with inotify. A new keyboard, mouse or
  touchpad:
  - is opened as root;
  - passes the same filter as the others;
  - is sent **only to the session that's on the monitor** (`RP_INPUT_ADD`, with the fd).

  Background sessions get nothing; they see it when they return to the monitor, because
  `sd_mon_show` opens everything again.
- **On unplug:**
  - `lord-sessiond` closes its copy;
  - the session sees `ENODEV` or `POLLHUP` on its own and **closes only that device**.
- **Leaving the monitor:** only happens when `lord-sessiond` asks (`RP_UNDISPLAY`). It's never
  inferred from a device error; before, unplugging a USB mouse left the screen black.

Tested in VM 123 with devices simulated through uinput:

- plugging, moving and unplugging with the session on the monitor;
- plugging back in;
- a mouse with a high-resolution wheel;
- a touchpad plugged in with the screen locked: it went to the login screen and not to `lord`'s
  session.

Switching users is just this: the other session stays alive in the background. The console is
put in graphics mode once (`KD_GRAPHICS`) and goes back to text when the service stops.

The card, the input and the monitor socket are never left open in the children (web
connections, workers): `sd_mon_child()` closes them before dropping root.

### No usable display driver

`sd_mon_open` (`remote/monitor.c`) waits up to 30 s for a card with KMS, a connector and a CRTC
(`drmcard.h`). A card that opens but has none of that, such as a render-only GPU, counts as no card:
lord-server could not start on it. lord-sessiond logs `no usable display driver`, creates
`/run/lordos/no-display`, writes what happened on tty1 (`tell_no_display`) and keeps running with no
monitor, so remote desktop still works if it is on. `lordos.service` stays active for that reason: read
the journal, not the unit's state. Once the driver is fixed, `systemctl restart lordos.service` picks the
display up; a driver that loads later than the wait is not picked up by itself.

### Boot splash

The boot splash (Plymouth, theme `lordos`; see `ISO.md` and `RASPBERRY.md`, "Boot splash") hands
the screen to LordOS without a black frame or a flash of the text console:

1. **`lordos.service` → `lordos-splash deactivate`**, the last `ExecStartPre` (on the Raspberry Pi
   after `wait-kms`): Plymouth stops drawing and drops DRM master, but its last frame stays on the
   screen. The unit has `TTYReset=no`: systemd's terminal reset switches tty1 back to text mode
   before every `Exec*` line, and with nobody holding DRM master the console would be drawn on top
   of the splash.
2. **lord-sessiond** opens the card, becomes DRM master (`SET_MASTER` works because Plymouth
   dropped it) and lends it to the login screen, or the autologin session, with
   **`RP_DISPLAY_KEEP`** (a flags byte after the fd count in `RP_DISPLAY`). The flag is only sent while
   `/run/lordos/first-frame` doesn't exist, that is, before the first frame of this boot: later the
   screen may hold another user's session, which must never stay visible.
3. **The session** (`kms_attach(..., keep)`) skips the black first modeset: `modeset_pending`
   makes the first `kms_flip` (CPU path) or `kms_set_fb`/`kms_flip_fb` (GPU path, direct scanout)
   do the `SETCRTC` with a real frame. Until then the splash is what the screen shows. Then it sends
   **`RP_SHOWN`** to lord-sessiond.
4. **lord-sessiond** creates `/run/lordos/first-frame` (also at once when there is no monitor).
5. **`lordos-splash-quit.service`** (wanted by `lordos.service`) waits for that file and runs
   `plymouth quit --retain-splash`: Plymouth closes its card without touching the screen, which
   already shows LordOS. Without the file after 35 s it ends Plymouth anyway. With no usable display
   (below) it ends Plymouth without `--retain-splash`, so the console shows.
6. Plymouth's own `plymouth-quit.service` (a plain `plymouth quit`) is ordered after it by
   `plymouth-quit.service.d/lordos.conf`, so it can't end the splash early. There is no
   `Conflicts=` with it (the display-manager way): starting `plymouth-quit.service` by hand would
   then stop LordOS.

lord-sessiond runs nothing new for this: Plymouth is only called from the units, with fixed
arguments (`/usr/lib/lordos/lordos-splash`). Without Plymouth installed or running, both steps do
nothing and the boot is the same as before (the first frame is then black, as always).

### The login screen (greeter)

It's a `lord-server --headless --greeter` running as `lordos-greeter`, unprivileged and without
an admin helper. Its runtime is in `/run/lordos/greeter` and its home in
`/var/lib/lordos-greeter`. It loads the special theme `themes/greeter` (React, the same engine):

- the date and a large clock;
- the chosen user's avatar with the «Password» field (→). When the screen is shown it preselects
  the locked user, otherwise the last user whose session was on the monitor (`last` in the `hello`
  reply, kept in `/var/lib/lordos/last-user` across reboots), then a user with an open session, then
  the first one. An avatar the person clicks always wins over a later `hello` reply;
- the other users below; a green dot marks those with an open session;
- bottom left, the machine's IP;
- bottom right:
  - the language (es, en, pt);
  - the keyboard, if the system has several (`/etc/default/keyboard`);
  - Restart and Shut Down. If there are open sessions, they ask for confirmation.
- A wrong password shakes the field. After many attempts, it asks you to wait.
- **Several displays (0.8.5):** the clock, the user and the password field go on the main display
  (the wallpaper still covers every display), instead of being laid out over the whole desktop and
  cut at the seam between two displays; they follow a display plugged in or out while the screen
  shows. Verified on VM 118 with two displays.
- **A monitor plugged in after boot (0.8.5):** the login screen follows the kernel's DRM uevents like
  a session does, without the system bus (`main.c`), so a board booted without HDMI (a Raspberry Pi 5)
  shows the login screen when a monitor is connected instead of staying without video.

To request things, the JS uses `lord.greeter.send(json)`. That function only exists in greeter
mode. The request travels like this:

```
greeter (JS) → RP_GREETER → lord-sessiond (root), over the login screen's socket
```

**Password verification is always done by the root process**, against `/etc/shadow` with
`crypt_r`. The JS never sees the hash. Requests and notices:

| Request (greeter → sessiond)       | Reply / notice (sessiond → greeter)                                       |
|------------------------------------|---------------------------------------------------------------------------|
| `{t:'hello'}`                      | `{t:'hello', mode, user, last, host, layouts, variants, lang, users:[…]}`  |
| `{t:'login', user, pass}`          | `{t:'ok'}` and the monitor switches to the session · `{t:'fail', wait}` · `{t:'error'}` |
| `{t:'power', a:'reboot'│'poweroff'│'suspend'}` | (`systemctl reboot` / `poweroff` / `suspend`, a closed list; `suspend` is the locked session's sleep time, SETTINGS.md) |
|                                    | `{t:'mode', mode:'login'│'lock'│'switch', user, dark, off, sleep, sleepLeft}` when shown (the last four: a lock's idle state, SETTINGS.md "Display off, lock, sleep") |

Failed attempts on the monitor are counted per user (`monitor:<user>`). The first two don't wait;
after that the wait is 1, 2, 4… seconds, up to 5 minutes.

### Boot time

When the first frame of a boot reaches the monitor, `lord-sessiond` logs the seconds since the kernel
started (`CLOCK_BOOTTIME`) with the LordOS version and build, and appends the same as one JSON line to
`/var/lib/lordos/boot-times.jsonl` (`record_boot_time`: root's, 0644, written whole and renamed, the last
200 boots, no user or host names). About LordOS shows this boot's line, found by the kernel's boot id
(`boot_time_load` in `ui/js_sys.c`: the file must be root's and not writable by others). The boot splash
shows its own timer (`tools/boot/plymouth/lordos/lordos.script`). How to check and compare boots:
[RESULTS.md](RESULTS.md), "Boot time: check it yourself".

**Boot time test (0.8.6, LordBench).** System Information → Performance can measure N boots in a row (2 to
10). The admin action `boot-test <n>` writes `/var/lib/lordos/boot-test` (root's, a fixed format:
`bench/boottest.c`) and the session restarts as usual. At each boot's first frame lord-sessiond records the
boot with `"test":k,"testOf":n` and, while boots are left, writes the file again and restarts 30 s later
(`boot_test_tick`). The login screen shows which boot it was and the countdown, with **Stop the test**; a login
there also stops it, and so does `boot-test-cancel`. Before restarting, lord-sessiond reads the file again and
restarts only if it still says what that boot wrote. A file older than 30 minutes (or from the future) is
discarded and `left` only goes down, so the test always ends; it is refused in the live session.

### Lock, switch user and log out

**The login screen runs only when it is shown (0.8.5).** It used to stay running behind the session
after sign-in, a whole `lord-server` (about 53 MB of its own memory, 78 MB of PSS, measured in a VMware
VM). Now `lord-sessiond` stops it 10 s after a session takes the monitor (`GREETER_LINGER_S`), and
`sd_greeter_get` starts it again for a lock, a user switch or a log out; its socket is up in 8–22 ms
and it reaches the monitor about 20 ms after it's asked for. On a lock or a switch the monitor is taken
from the session **first** (`leave_for_greeter`: input revoked, its framebuffers removed, the screen
dark), and only then is the login screen asked for, so an unlocked desktop never stays on the screen
while it starts. Taking the monitor away is the slow part on some drivers: 1.18 s on VMware's
`vmwgfx` (removing the framebuffer on the screen turns the output off), the same as before this change.
Most of that was the input close; with it moved to a thread (see "The monitor" above) the lock to the
sign-in screen measured 141 ms on a VMware Fusion VM.
The log says both: `monitor: taken from <user> in N ms` and `monitor: the login screen (lock), N ms
after the screen went dark`.

- **Lock screen:** ❖L, ❖Ctrl+Q, or the logo menu → «Lock Screen»; also "require password" after the
  display turns off, and (0.8.8.4, issue #36) before the computer sleeps: the session on the monitor
  holds a logind delay inhibitor and lets it go only once `RP_UNDISPLAY` arrives (`hw/sleeplock.c`,
  SETTINGS.md "Display off, lock, sleep"), so the computer never sleeps with the desktop on the screen.
  1. The session sends `RP_SESSION {"a":"lock"}` to all its viewers.
  2. The monitor shows the login screen with that user.
  3. Browsers connected to that session are also cut off and go back to the login
     (`/?e=3`).
- **Lock from logind (0.8.8.8; built, not yet checked on the VM):** `loginctl lock-session`, `loginctl lock-sessions` and
  any program or admin that calls logind's `LockSession` make logind emit `Lock` on the session's
  `org.freedesktop.login1.Session` object; logind allows that only to root, the session's owner or
  whoever its polkit rules let through. The session hears it on the system bus (`hw/logindlock.c`,
  believed only from the connection that owns `org.freedesktop.login1`) and locks as ❖L does (idle
  `{stage:"lock"}` for the theme). Which `Lock` counts (`hw/lockstate.c`, tested by
  `build/logindlock-test`):
  - the desktop's own logind session (`GetSessionByPID` of `lord-server`): only a `Lock` of that path,
    and its `LockedHint` follows the lock (`SetLockedHint`, which logind allows the session's owner):
    true once a lock is sent from the monitor, false when the monitor shows the session again, so
    `loginctl show-session -p LockedHint` tells;
  - in none, which is the case today (`lord-sessiond`'s sessions are not logind sessions, see "Users"
    below): a `Lock` of any session of the same user (its `User` property, asked with
    `Properties.Get`), so `loginctl lock-sessions` and `loginctl lock-session <id>` of one of the user's
    sessions (an SSH login, the user manager's) lock the desktop. There is no session to put
    `LockedHint` on;
  - **`Unlock` is never obeyed.** Only the password on the login screen gives the monitor back
    (`lord-sessiond`, root). Unlocking from logind would be a new way for root to hand the screen
    over, so it waits for the maintainer's review; the session only logs it.

  Before sleep, logind's `PrepareForSleep(true)` with "require password" set to Immediately already
  locks first under a delay inhibitor (`hw/sleeplock.c`, above).
- **Switch User…**: `{"a":"switch"}`. Shows the login screen with another user selected. The
  previous session stays alive in the background and you return to it by logging in again.

  ![Switch user](login-switch-user.png)
- **Log out:** ends that user's `lord-server` and the monitor goes back to the login screen.
- **Restarting `lord-sessiond`:**
  - sessions stay alive (`KillMode=process`), and on return you get back in through the login
    screen;
  - `lord-netd` keeps running too and the new `lord-sessiond` adopts it: the network is not cut;
  - the login screen does **not** survive: it keeps no state, so `lord-sessiond` stops every
    process of `lordos-greeter` when it stops, and any left over (after a crash) when it starts,
    and starts a fresh one on demand. Before 0.8.0-beta the old greeter stayed ("remains running
    after unit stopped") and, after a package upgrade, kept the old binary;
  - `lordos.service` has `DelegateSubgroup=supervisor`: without it, systemd couldn't start the new
    main process while live processes remained in the delegated tree ("Device or resource busy");
  - autologin is applied only once per machine boot (`/run/lordos/autologin-done`), so a service
    restart doesn't unlock a locked session.
- **Linux app windows across a lock or a switch:** leaving the monitor tears the session's GPU
  context down and coming back makes a new one. Every window is imported again in the new context
  and, while the session is off the monitor, a window whose buffer the CPU can't read keeps its
  last frame. Before 0.8.1, Chromium's window came back black (`GPU.md`, "When the GPU context is
  re-created"; `tests/gpu_cycle.sh` repeats the off/on cycle).

In the SDK: `session.lock()`, `session.switchUser()` and `session.managed()`. The `default` and
`classic` themes show those items only if `managed()` is true.

### Restart and Shut Down from a session

Since 0.8.1 the crown menu ends with **Restart…** and **Shut Down…**, after Lock Screen, Switch
User… and Log Out; the Lord Classic theme has them behind a **Power** button at the bottom of its start
menu. Before, they were only on the login screen. Sleep is not offered: suspend and resume with the
card lent to a session has not been tested on real hardware yet.

| | |
|---|---|
| ![crown menu](session-power-menu-light.png) | ![crown menu, dark](session-power-menu-dark.png) |
| ![restart confirmation](session-power-restart-light.png) | ![restart confirmation, dark](session-power-restart-dark.png) |
| ![Lord Classic start menu, Power](session-power-classic-light.png) | ![Lord Classic, dark](session-power-classic-dark.png) |

- **The confirmation** (`sdk/power.jsx`, `<PowerDialog>`, in the `modal` layer; a theme renders it
  inside its `ThemeProvider`) says what will happen and counts down 60 seconds; at zero it does it,
  as if confirmed. Cancel, Escape or a click outside stops it. It lists the apps that are open
  (they are closed; there is no "unsaved changes" signal to read yet) and the other users with a
  session open.
- **The request:** `session.restart()` / `session.shutdown()` → `lord.session.power('reboot'|'poweroff')`
  (`js_sys.c`), which only knows those two words and sends a fixed `RP_SESSION`
  `{"a":"power","p":"reboot"|"poweroff"}`. lord-server runs nothing.
- **The decision is lord-sessiond's** (`session_power` in `sessiond.c`), and only for the session
  that is on the monitor (its connection is the only one it reads requests from):
  - the user is alone (no other user has a session): any user may restart or shut down;
  - other users have sessions open: only an administrator (`sudo`, `admin` or `wheel`, the same
    rule as the admin actions in `admin.c`). The sheet says so beforehand and hides the button; if
    it changed in between, lord-sessiond answers `{t:'power', ok:false, why:'admin'}` over the login
    screen's channel (`RP_GREETER_IN`) and the sheet shows the refusal.
  - then the same closed path as the login screen: `systemctl reboot` or `poweroff`, fixed path,
    fixed environment, logged as `reboot requested by <user> (<n> other session(s) open)`.
- **Only on the monitor's session** (0.8.5): the items appear only while the session holds
  lord-sessiond's seat connection (`lord.session.onMonitor()`, which `session.canPower()` checks). A
  session seen only through the remote desktop isn't offered them; before 0.8.5 it showed the
  sheet and nothing happened at the end of the countdown.
- **Tested on VM 122:** the menu items, the sheet in light and dark, the countdown, Cancel and
  Escape (no restart after the minute), the Lord Classic Power button, a real Shut Down (`qm start` back
  to the login screen) and a real Restart (back to the login screen). The refusal with other users
  logged in was checked in the code path only: it needs a second account's password on the VM.

### Session restore

Each window comes back on its desktop, with the desktop that was in view ([DESKTOPS.md](DESKTOPS.md), "What is kept").

The apps open at the end of a session come back at the next login, each window where it was.
Settings → Session → **Reopen windows when you log in**: Always (the default), Ask first or Off.

| How the session ended | Always | Ask first | Off |
|---|---|---|---|
| Log out, Restart, Shut Down, `systemctl`, a signal to `lord-server` | reopens | asks | nothing |
| Power cut, crash, `SIGKILL` | asks ("did not shut down properly") | asks | nothing |
| Everything was closed | clean desktop | clean desktop | clean desktop |

**What is saved.** `sdk/sessionrestore.jsx` keeps a debounced snapshot of the windows store (2 s, only when it
changed) in `settings.sessionSaved`, in `settings.json`, which is already on `lord.fs`'s deny list; no new file.
That write and the logout's are `lord.settings.save(text, true)`: fsynced, file and folder, because they must
survive the power cut they are for. Each entry is a typed Dock id (`dockIdOfWindow`), a frame, maximized,
minimized and the snap zone; never an Exec line, argv, environment, pid, title or a Linux app's document. A
LordOS app's `args` are kept only for the fields listed per app in `sdk/sessionmodel.js`, without `nonce`. Left
out: windows with no `.desktop`, hidden apps, Force Quit, About This App, the installer, the sandbox probe,
Screenshots' editor, picture-in-picture. The file can be written by any program the user runs, so the list is
validated again when it is read, the same rules (`server/tests/session_model_test.mjs`).

**The marker.** `sessionSaved.clean` is false from the moment a session starts. It turns true in `flushSession()`
(`session.quit`, Search's Log Out, the power dialog before it asks lord-sessiond; a refusal undoes it) or in
`__lordBeforeExit`, which `main.c` calls after the main loop. A session that never gets there leaves it false,
and that is what "unclean" means. Switching theme is not an end: the marker says `'theme'` and `themeReopen`
does the work.

Worker threads start before `main.c` blocks `SIGTERM`, so a signal used to end the process on one of them
without leaving the loop (seen on VM 125: gone in under 300 ms, nothing written). A handler now forwards it to
the main thread's signalfd, so every signal stop runs the normal end of a session, including `term_shutdown`.

**Coming back.** Up to 12 windows, 450 ms apart, through the Dock's own path (`launchDockItem`: `launchApp`,
`lord.apps.launchPackage`, `lord.apps.launch(desktop id)`). A LordOS app opens with `wm.open` and its frame. A
Linux window is a new process, so its saved frame waits by Dock id and the first new window of that app takes it
(90 s at most). What is already open is not opened twice (Open at Login, the Terminal's live sessions). The
window that was in front ends in front. Nothing runs on a live system, the login screen or in an isolated app.

**At the end of a session** `wl_close_all()` sends every window `xdg_toplevel.close` (or `WM_DELETE_WINDOW`) and
waits up to 3 s for those apps to leave, before `wl_stop()`. Without it a browser records a crash. An app that
doesn't answer (seen with a stopped one: 3.4 s in all) or asks something back is cut as before.

**Browsers.** Settings → Session → **Browser tabs** runs the closed action `browser-restore on|off`
(`hw_admin.c`), which writes fixed text only: `/etc/chromium/policies/recommended/lordos-session.json`
(`RestoreOnStartup: 1`, recommended, so the user can still change it in the browser) and
`/etc/firefox-esr/lordos-session.js` (`browser.startup.page` 3); `off` removes both, and so does `postrm`.
These are machine-wide: the last session to apply its setting wins. On VM 125, Chromium and Firefox ESR
brought their pages back after a logout and a reboot with no crash notice. After a forced power-off Firefox
came back alone; Chromium shows its own "Restore pages?" and its button brought the tab back: LordOS can't
answer that for it, and an app with no such feature loses what it had open (only the window returns).

What doesn't come back: the state inside a Linux app, a Files window's current folder when it wasn't the one it
opened with (only `args` are saved), a Terminal's shells after a reboot (a new one opens in its window).

## cgroups: one subtree per session

Each app in a session goes into its own cgroup v2 (a «scope») so monitoring can attribute traffic
and hardware and so limits can be applied (see [PRIVACY.md](PRIVACY.md)). `lord-server` runs
without root, so `lord-sessiond` **delegates** a subtree to it:

- `lordos.service` has `Delegate=yes`. At startup, `lord-sessiond` moves itself to the
  `supervisor` leaf (`cgroup_service_init`, `server/src/sec/cgroup.c`), so the service root has
  no processes and can enable `cpu memory pids` for its children.
- When a user's session is created (`spawn` in `sess.c`), the child —still root, before
  `lord-server`'s `execve`— calls `cgroup_delegate_session(uid, gid)`: it creates
  `lordos-session-<uid>/server`, moves itself there and hands the subtree to the user (`chown`).
- `lord-server` creates its apps' scopes as siblings of `server`, without root:
  `app-lordos-lpk-<id>.scope` (isolated `.lpk` apps) and `app-lordos-<id>.scope` (Linux apps).
- **Update (root-owned scopes):** the subtree is no longer chowned to the user. The scopes are
  created by `lord-sessiond` (a root child on `/run/lordos/cgroup.sock`, `remote/cgsup.c`) when
  `lord-server`, or a child it forked right before exec, asks to be placed; all of them, and
  every `cgroup.procs`, are root's. After a restart the new `lord-sessiond` hardens the subtrees
  of the sessions that survived. See `PRIVACY.md`, «Root-owned app scopes».
- The login screen and the web connections stay in `supervisor`.

```
lordos.service
├─supervisor                 lord-sessiond, login screen, web connections
├─lordos-session-1000        (lord's)
│ ├─server                   its lord-server and its helper
│ ├─app-lordos-lpk-evil.scope
│ └─app-lordos-featherpad.scope
└─lordos-session-1001        (bob's)
  └─server
```

Tested in VM 122: `lord` on the monitor, `bob` logging in through the browser (new session in
`lordos-session-1001`) and `lord` through the browser (the same session as the monitor); fast
user switching from `lord` to `bob` and back, with `lord`'s apps (FeatherPad and an isolated app)
alive. With `KillMode=process`, a service restart leaves the sessions in their subtrees.

Without `Delegate=yes` (or `lord-server` run standalone with sudo, as in development) everything
works as before: apps run without a scope and a warning goes to the log.

## Autologin

It's configured in `/etc/lordos/session.conf`: `AUTOLOGIN=user`, or empty to use the login
screen.

- **Installed system:** the installer writes an empty `AUTOLOGIN=`. You log in through the login
  screen.
- **Settings → Users → «Automatic login»:** off by default; it's changed by the
  `user-autologin` action.
- **Live ISO:** `AUTOLOGIN=lordos`. The `lordos` user has no password, so after locking you get
  back in with the field empty. This only applies:
  - on the monitor;
  - to the autologin user;
  - never from the network.

## Settings → Users

![Users](login-settings-users.png)

The list comes from `lord.users.list()`, read-only: accounts with uid 1000–59999 and a real
shell. It shows the avatar, whether the user is an administrator and whether they have an open
session. Everything that changes something goes through closed `admin.c` actions
(`server/src/users/users_admin.c`), validated on the root side:

| Action            | Argument (lines)                           | Who                                           |
|-------------------|--------------------------------------------|-----------------------------------------------|
| `user-add`        | name, full name, password, admin           | administrator                                 |
| `user-del`        | name, keep folder (0/1)                    | administrator; not one's own nor the last admin |
| `user-passwd`     | name, new, current                         | one's own, with the current one; someone else's, an administrator |
| `user-admin`      | name, 0/1 (`sudo` group)                   | administrator; not the last admin             |
| `user-avatar`     | name, color `#rrggbb`, image               | one's own, or an administrator                |
| `user-autologin`  | name or empty                              | administrator                                 |

- **Passwords:**
  - they're written with `chpasswd` through standard input, never in argv;
  - the helper wipes the argument from memory after using it;
  - the SDK doesn't keep it in the "in progress" notice (`quiet`).
- **Avatar image:**
  - it must be a path in the requester's home folder;
  - it's read with their permissions (`setfsuid`), so root reads nothing the user couldn't read;
  - PNG or JPEG only, up to 4 MB.
- **Avatars:** they're kept in `/var/lib/lordos/users/<u>.json` (the color) and `<u>.png` (the
  image). The login screen can read them; each user's home, it can't.
- **New user:**
  - it is created with the common folders in the system language (Escritorio, Documentos,
    Descargas, Música, Imágenes and Videos; in English or Portuguese if that is the language), its
    `~/.config/user-dirs.dirs` and `user-dirs.locale` (xdg-user-dirs), plus `settings.json` and
    `locale.conf`. The list lives in one place, `server/src/users/homedirs.c`, which the installer
    uses too; it matches `lordos-live-prep`. Test: `build/homedirs-test`;
    ![new user ana, created with her folders](system-new-user.png)
  - the username is suggested from the full name;
  - it joins `pipewire` and `plugdev`, and `audio`, `video` and `render` when those groups exist:
    `lord-sessiond` sessions have no logind seat, so there are no `uaccess` ACLs, and without
    them a new user only got PipeWire's dummy output (integration QA, VM 122). `input` is not
    added: `lord-sessiond` lends the input devices to the session on the monitor.

## Security

| Process                           | Runs as         | What it can do                                                   |
|-----------------------------------|-----------------|------------------------------------------------------------------|
| `lord-sessiond` (main)            | root            | reads shadow, opens and lends the card and input, creates sessions, reboots or shuts down (login screen, or the monitor's session under the rule above); doesn't read pixels |
| `lord-netd`                       | root            | only the `net-*` actions of `admin.c` (NETWORK.md); talks only to the sessions' root helpers (`netd.sock` 0600) |
| login screen                      | `lordos-greeter` | draw and request; no admin helper and no access to homes        |
| session                           | the user        | the usual (the `admin.c` helper with its closed list); the card and input only while it's on the monitor |
| web connection                    | root → user     | drops root as soon as it knows whose session it is (REMOTE.md)   |

- **Messages from the sessions:** lord-sessiond's connection (`RP_SEAT`) doesn't receive the
  screen. It only reads short JSON of up to 4 KB and skips everything else without interpreting
  it. Screen rectangles are processed by the web bridge, which already runs as the user.
- **Binaries:** sessions are only created with a `lord-server` owned by root that nobody else can
  write. `insecure_dev=1` in `/etc/lordos/remote/config` allows it in a development VM.
- **Locking really takes the session off the monitor:** it isn't a layer on top of the desktop.
  If the user's session hangs, the login screen keeps working.

## Files

| Path                                   | What it is                                                |
|----------------------------------------|-----------------------------------------------------------|
| `server/src/remote/sessiond.c`         | the main process                                          |
| `server/src/remote/monitor.c`          | the monitor: lend and revoke the card and input           |
| `server/src/remote/sess.c`             | accounts, passwords, waits, creating and finding sessions |
| `server/src/remote/mirror.c`           | session side: viewers, lent monitor, size, requests       |
| `server/src/remote/proto.h`            | the local protocol                                        |
| `server/src/users/users_admin.c`       | `user-*` actions                                          |
| `server/src/users/homedirs.c`          | common folders and `user-dirs.dirs` (Users and the installer) |
| `server/src/remote/netsup.c`           | starts, supervises and adopts `lord-netd` (NETWORK.md)       |
| `/run/lordos/netd.sock`, `netd.pid`, `netd.enabled` | the system network (NETWORK.md)                 |
| `server/src/sec/cgroup.c`              | per-session cgroup subtree and per-app scopes (`Delegate=yes`) |
| `themes/greeter/`                      | the login screen                                          |
| `apps/settings/Users.jsx`              | Settings → Users                                          |
| `/run/lordos/status.json`              | sessions, who's on the monitor and who's connected over the network |
| `/run/lordos/first-frame`              | the first session frame of this boot is on the monitor (boot splash hand-off) |
| `/run/lordos/no-display`               | no usable display: the boot splash ends without keeping its frame |
| `tools/boot/lordos-splash`, `tools/boot/systemd/` | the boot splash hand-off: `deactivate`, `quit` and the units |
| `/etc/lordos/session.conf`             | `AUTOLOGIN=`                                              |
| `server/src/lordlog.c`                 | the log folders and the `lord-log` child that stamps lord-sessiond's lines (0.8.8.8) |
| `/var/log/lordos/lord-sessiond.log`    | lord-sessiond's log (root, 0640; also still in the journal)  |
| `/var/log/lordos/users/<user>/`        | each user's log folder, made by lord-sessiond at each login before the session starts (SYSTEM.md, "Log files") |

## Screenshots

- ![session on the monitor compositing on the GPU (llvmpipe, LORD_GPU=1) after going through the login screen](login-session-gpu.png)
- ![wrong password](login-error.png)
- ![locked screen](login-locked.png)
- ![logo menu: lock and switch user](login-menu.png)
- ![bob on the monitor while lord's session stays open](login-session-second-user.png)
- ![restart with open sessions](login-restart.png)
- ![new user](login-new-user.png)
- ![delete a user](login-delete-user.png)

## Pending

- **Multiple monitors:** since 0.8.0 the session drives every display of the lent card
  (`display_attach` → `outputs_apply` in `main.c`, see [DISPLAYS.md](DISPLAYS.md)). The lend/revoke
  cycle with several real monitors was exercised on one PC with a Radeon RX 580 (amdgpu) and three monitors:
  it brought 0.8.8.6's login screen on the chosen main display and 0.8.8.7's hand-off with a black frame and
  CLOSEFB, so DisplayPort monitors keep their signal across lock, logout and login. Other cards and laptop
  panels are untested.
- **Text consoles:** Ctrl+Alt+F2 doesn't switch consoles while LordOS is running, as before.
- **Choosing the avatar image:** today it's a hand-typed path; the file picker is missing.
- **Testing with more real cards:** in the VMs the GPU is tested only with `LORD_GPU=1` and llvmpipe; on
  real hardware only the Radeon RX 580 above (amdgpu). Other AMD, Intel and NVIDIA cards and the live USB on
  them are untested.
