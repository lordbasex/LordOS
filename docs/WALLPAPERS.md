# Moving wallpapers

Short loops behind the desktop and the login screen, chosen in Settings → Wallpaper. They exist only with the hardware for
them: there is no software mode.

## The rule

A moving wallpaper plays only when **both** hold:

- the GPU composes the session (`lord.media.composition()` is `gpu`);
- the hardware decodes the file's format (`lord.media.hwDecode()`).

If either is missing, the section is disabled with one line, "Moving wallpapers need a compatible GPU.", and a chosen
wallpaper shows its still poster. H.264 is used only where it is decoded in hardware. The login screen follows the same
rule and otherwise shows its still image. The decisions are in `sdk/motionmodel.js`, tested without a screen by
`server/tests/motion_model_test.mjs`.

A virtual machine's GPU never qualifies (`hwDecode().virtualGpu`). On a V4L2 decoder HEVC counts only if the FFmpeg in use
can drive it (`drmprime` from `lm_hwprobe.c`): Debian's FFmpeg can't, Raspberry Pi's can.

## Format

HEVC first: it is what a Raspberry Pi 5 decodes in hardware. Each item has `1080.hevc`, `2160.hevc` when the source is 4K,
and `1080.h264` for machines that decode only that. `pickVariant` takes HEVC before H.264, then the smallest file that
covers the display.

Clips last 6 to 10 s. The loop is closed by fading the second after the clip's end over its first second. Nothing is
upscaled or interpolated: frame rate and size never exceed the source's. Of the sources chosen, four are real 60 fps and
the rest 12.5 to 30 fps. No 120, 144 or 240 fps source with a clear license was found.

## When it pauses

`sdk/motion.jsx` pauses the decoder, and `pauseReason` says why: Reduce motion, a fullscreen window, every display showing
it covered by a window, five minutes with no input, the battery at 20 % or less and discharging, or the CPU above 85 % for
two readings 5 s apart. The login screen pauses after five minutes with no input.

One video per file, however many displays show it. The still wallpaper stays drawn underneath, so a failure, a missing
file or a helper crash just shows it.

## Files and downloads

The catalog `assets/wallpapers/motion.json` ships in the package: ids, credits, sizes and a SHA-256 for every file. Its
thumbnails and posters ship next to it (`assets/wallpapers/motion/`, 6.6 MB with the built-in clip).

Videos are downloaded on demand by closed admin actions (`server/src/hw/wallpaper_admin.c`):

| Action | Who | What |
|---|---|---|
| `wallpaper-get "<id> <variant>"` | anyone | downloads a catalog file to `/var/lib/lordos/wallpapers/<sha256>.mp4` |
| `wallpaper-remove "<id> <variant>"` | admin | deletes it |
| `wallpaper-login "<id> <variant>"` or `default` | admin | the login screen plays it (`login.mp4`) or the built-in one |

JS names an entry, never a URL or a path. The base must be the project's GitHub owner on `raw.githubusercontent.com`, the
bytes go through `fetch_batch` (a child as `nobody`, in the Store's scope, so the firewall sees `lordos:store`), and a file
whose size or hash isn't the catalog's is deleted. `lord-media` keeps its sandbox and receives an open file, and
`media_open_input` accepts those two system folders besides the user's own files. `server/tests/wallpaper_find_test.c` covers the lookup.

An added video stays in the user's files. It needs at most 10 s and a format this machine decodes in hardware.

## The login screen

The greeter can't read a user's settings, so the choice reaches it as a file: `/var/lib/lordos/wallpapers/login.mp4`, else
the built-in `crown.hevc.mp4` or `crown.h264.mp4`.

Before 0.8.7 the login screen composed on the CPU: `lordos-greeter` couldn't open `/dev/dri/renderD128`. It now joins
`render` and `video` (`install.sh`, the ISO, the Pi image, migration 006). That gives the login screen's process the GPU
driver's attack surface; it already ran Mesa in software.

## The Raspberry Pi 5

Hardware HEVC needs Raspberry Pi's FFmpeg, pinned in `/etc/apt/preferences.d/raspberrypi` (migration 006 adds the pin on
upgrades, and the next `apt` update brings the packages). That FFmpeg opens the decoder by path, so for this one job,
`MK_PLAY` with `hwaccel=v4l2request`, `lord-media` may open two exact device files found from sysfs before the sandbox,
reads `/sys/class`, `/sys/bus` and `/run/udev`, and gets the V4L2 and media controller ioctl classes.

## Measurements

| Machine | Case | Result |
|---|---|---|
| Pi 5, login, trial build | 1080p60 H.264, software decode, CPU composition | about 156 % of one core |
| Pi 5, login, 0.8.7 | 1080p60 HEVC, hardware decode, GPU composition | `lord-media` 41 %, `lord-server` 147 % of one core, 30 s, SoC 52 °C with its case |

Decoding is in hardware; each frame is then read back and converted to BGRA in `lord-media`, and `lord-server` paints it. That copy is why the total stays high. Showing the decoder's buffer directly on the GPU is the next step, in 0.8.8.

Settings offers 1×, 0.75×, 0.5× and 0.25×. A slower speed decodes fewer frames per second, and it applies to the desktop, not to the login screen.

Choosing a still wallpaper turns the moving one off, since it would stay in front. A new player can start while the last one still holds the decoder, so a stop nobody asked for is retried four times, half a second apart; the tile shows a spinner until the video plays.

A moving wallpaper plays with `hwOnly`: if the hardware decoder doesn't start, or fails, `lord-media` stops rather than decoding in software, and the still image stays.

## The collection

Files live in a separate repository, `LordOS-wallpapers`, because 353 MB of video doesn't belong in the code's history. Its
README credits each clip; licenses come from each file's own page. Only public domain, CC0, CC BY and CC BY-SA sources are
used.
