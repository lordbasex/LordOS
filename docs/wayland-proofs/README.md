# Wayland and X11 conformance: proof with real programs

Screenshots of real programs on a headless `lord-server` (1280×800, CPU path, `LORD_GPU=0`), taken on this
development machine (Debian 13) on 2026-11-22, after the conformance work (`docs/WAYLAND.md`, "Conformance";
`docs/wayland-conformance.json`). Each run uses its own runtime folder and home and never touches a session.
Nothing was installed for them; programs not installed here are listed at the end.

- Wayland: `node tools/wl-proof.mjs` (each program with `WAYLAND_DEBUG=1`; `proofs.json` has what its log held:
  the requests that matter and any protocol error). Saved as 256-colour PNGs.
- X11: `tools/wl-conformance/x11-proofs.sh <repo> <outdir>` (Xwayland 24.1.6; saved at 960×600, 128 colours).

## Wayland

| Image | Protocol | Program | What it shows |
|---|---|---|---|
| `wl_subcompositor-wp_viewporter.png` | `wl_subcompositor`, `wp_viewporter` | `build/wlproof-client` (`server/tests/wlproof_client.c`, purpose-built: no installed app shows this in pixels) | A 400×300 window, its right third transparent. Red was made before green, and `place_above(red, green)` puts red over green. Blue was `place_below(parent)`: it is under the window and shows only through the transparent third. A 64×64 buffer in four colours is cropped to its white quadrant and scaled to 100×100 (`set_source` + `set_destination`): one white square |
| `xdg_wm_base-foot.png` | `xdg_wm_base`, `zxdg_decoration_manager_v1` | foot 1.21.0 | The terminal mapped and focused with the theme's frame. Its log: `pong` to the ping sent on focus, each configure acked once (`invalid_serial` is enforced now), `set_mode` on its decoration, no protocol error |
| `xdg_wm_base-gtk4-zenity.png` | `xdg_wm_base` | zenity 4.1.90 (GTK 4.18.6, libadwaita) | A GTK 4 dialog with its own decorations. Its log: `pong`, configures acked once, the xdg_surface made before any buffer (`invalid_surface_state` and `unconfigured_buffer` are enforced now), no protocol error. (The dialog's text names subsurfaces and the viewporter; the log only proves xdg-shell) |
| `xdg_wm_base-qt-featherpad.png` | `xdg_wm_base`, `zxdg_decoration_manager_v1` | FeatherPad 1.6.1 (Qt 6.8.2, `QT_QPA_PLATFORM=wayland`) | A Qt 6 editor with the theme's frame. Its log: `pong`, configures acked once, no protocol error |
| `linux-dmabuf-vkcube.png` | `zwp_linux_dmabuf_v1` | vkcube 1.4.304 (Mesa 25.0.7, Vulkan WSI) | The spinning cube after about 650 commits: its buffers pass the new `out_of_bounds` checks, no protocol error |
| `wp_fifo_manager_v1-wp_commit_timing_manager_v1-wp_linux_drm_syncobj_manager_v1-vkcube.png` | `wp_fifo_manager_v1`, `wp_commit_timing_manager_v1`, `wp_linux_drm_syncobj_manager_v1` | vkcube 1.4.304 (Mesa 25.0.7, Vulkan WSI, FIFO present mode) | The spinning cube after about 5 s. Its log: about 320 frames, each with `wp_fifo_v1.set_barrier` and `wait_barrier` (Mesa's FIFO without frame callbacks), a `wp_commit_timer_v1.set_timestamp`, and explicit-sync `set_acquire_point`/`set_release_point` on 12 imported timelines; no protocol error (taken 2026-11-22, branch `feat/wl-conformance-complete`) |
| `xdg_system_bell_v1-wp_color_manager_v1-foot.png` | `xdg_system_bell_v1`, `wp_color_manager_v1` | foot 1.21.0 | foot after printing BEL: its log has `xdg_system_bell_v1.ring` on its surface (its default `bell.system=yes`; LordOS plays its alert sound unless Do Not Disturb is on) and a parametric sRGB description from `create_parametric_creator` set on its surfaces (`set_image_description`); no protocol error (taken 2026-11-22, branch `feat/wl-conformance-complete`) |
| `egl-es2gears.png` | `wl_surface`, `xdg_wm_base` | es2gears_wayland (mesa-utils 9.0.0) | An EGL/GLES2 client drawing (about 330 commits), no protocol error |
| `chromium.png` | `xdg_wm_base`, subsurfaces, viewporter, data device | Chromium 156.0.8078.79 (`--ozone-platform=wayland`) | A page in Chromium with its own tab strip. Its log: `pong`, no protocol error. (Its idle-notification prompt was answered by the harness, so the window had the keyboard) |

## X11 (Xwayland)

| Image | Item | Program | What it shows |
|---|---|---|---|
| `x11-xwayland.startup.png` | `xwayland.startup` | xterm 398, xdpyinfo | The window manager's log line "0x40000c is surface 2 by xwayland-shell-v1": the serial association in use |
| `x11-ewmh.net_supported.png` | `_NET_SUPPORTED`, `_NET_WORKAREA`, `_NET_CLIENT_LIST` | xterm 398, xprop | The full `_NET_SUPPORTED`, `_NET_WORKAREA = 0, 28, 1280, 690`, `_NET_CLIENT_LIST`, `_NET_CLIENT_LIST_STACKING`, `_NET_ACTIVE_WINDOW` |
| `x11-icccm.wm_hints.png` | `WM_HINTS` urgency | two xterm 398 (`*bellIsUrgent`), xprop | An xterm in the background rang its bell: "The urgency hint bit is set", `_NET_WM_STATE_DEMANDS_ATTENTION`, and the theme's "attention" event |
| `x11-icccm.wm_normal_hints.png` | `WM_NORMAL_HINTS` | xterm 398, xprop, xwininfo | Opened at 884×582; with minimum 560×380 and maximum 760×520 set it became 760×520; asking for 10×3 characters it got 560×380 |
| `x11-icccm.transient_for.png` | `WM_TRANSIENT_FOR` | two xterm 398, xprop | The dialog xterm's `WM_TRANSIENT_FOR` is the other xterm, and the theme got its "parent" event |
| `x11-ewmh.wm_icon.png` | `_NET_WM_ICON` | zenity 4.1.90 with `GDK_BACKEND=x11`, xprop | zenity's 16×16 `_NET_WM_ICON`, the theme's "icon" event (size 16), and zenity's own icon as its Dock entry |
| `x11-ewmh.frame_extents.png` | `_NET_FRAME_EXTENTS`, `_NET_WM_PID`, `_NET_WM_STATE` | xterm 398, xprop | `_NET_FRAME_EXTENTS = 0,0,0,0`, `_NET_WM_STATE_FOCUSED`, `WM_STATE` Normal, `_NET_WM_PID`, `_NET_WORKAREA` |
| `x11-always-on-top.png` | `_NET_WM_STATE_ABOVE`, `_NET_CLIENT_LIST_STACKING` | two xterm 398, xprop, xwininfo; `build/x11-conformance-client netstate` sends the request (as `wmctrl -b add,above` would: xprop can only set properties, and wmctrl isn't installed) | The xterm that asked for ABOVE (`0x40000c`) stays drawn over a normal xterm opened after it, which has the keyboard (`_NET_ACTIVE_WINDOW = 0x80000c`); its `_NET_WM_STATE` is ABOVE, it is last in `_NET_CLIENT_LIST_STACKING`, and the theme got its `layer` event (taken 2026-11-22, branch `feat/always-on-top`) |

## Not shown with a real program

- **No installed program sends these:** `_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`, the MODAL/SKIP_TASKBAR/SKIP_PAGER/BELOW state
  requests, the utility window type, `_NET_REQUEST_FRAME_EXTENTS` (xdotool and wmctrl aren't installed). They are
  checked by `build/x11-conformance-client` only.
- **Needs hardware:** a real graphics tablet and a VR headset (the conformance run simulates both through
  `lord-server --test-hooks`, `server/src/testhooks.h`: a tablet's tools, wheel, pad and `set_feedback`, and drm-lease's
  whole flow, are checked there on fake devices), and a touchscreen (`wl_touch.orientation`; its logic is covered by
  `build/tp-test`). Chromium, zenity (GTK 4) and FeatherPad (Qt 6) bind several of the newer globals (among them xdg-toplevel-drag,
  content-type, the tablet manager and the system bell), but nothing in these runs uses them visibly (no tab dragged,
  no tablet); xdg-toplevel-tag, xdg-session-management, pointer-warp and ext-background-effect are used by no installed
  program. `build/wl-conformance-client` checks all of them.
- **Not installed here:** Firefox, mpv, the GTK 3/4 demos, `wayland-info`, `weston-simple-*`, xdotool, wmctrl, xeyes,
  xclock, xlogo, xcalc, yad, xwd, ImageMagick.
