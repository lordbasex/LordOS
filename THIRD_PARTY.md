# Third-party components

LordOS's own code is released under the MIT License (see `LICENSE`). It bundles or links
the components below, each under its own license.

## Bundled in this repository (`third_party/`)

| Component | Used for | License |
|---|---|---|
| QuickJS | JavaScript engine for the UI | MIT (`third_party/quickjs/LICENSE`) |
| stb_image, stb_image_write, stb_truetype | image decoding/encoding, font rasterization | MIT or public domain (dual) |
| Monocypher 4 | vault and keyring cryptography | CC0-1.0 or BSD-2-Clause (`third_party/monocypher/LICENCE.md`) |
| EFF large wordlist, SecLists common passwords | passphrase generator, weak-password check | CC BY 3.0 (EFF), MIT (SecLists, `third_party/wordlists/LICENSE-SecLists`) |

## Data files

| Data | Used for | License |
|---|---|---|
| DB-IP Lite Country (`assets/monitor/geoip-country.mmdb.xz`) | offline IP→country in Monitor | CC BY 4.0 — IP geolocation by DB-IP (https://db-ip.com) |
| Natural Earth (`assets/monitor/land.bin`) | world map in Monitor | public domain |
| Unicode Character Database / CLDR (generated tables) | text, bidi, emoji names | Unicode License v3 |
| Inter (`assets/fonts/Inter-*.ttf`) | UI text | SIL OFL 1.1 (`assets/fonts/Inter-LICENSE.txt`) |
| Plus Jakarta Sans 2.7.1 (`assets/fonts/PlusJakartaSans-ExtraBold.ttf`, `-Medium.ttf`) | the LordOS wordmark: live text in the UI (fontFamily `brand`) and outlined in `assets/brand/*.svg` | SIL OFL 1.1 (`assets/fonts/PlusJakartaSans-LICENSE.txt`) |
| JetBrains Mono (`assets/fonts/JetBrainsMono-*.ttf`) | terminal and code text | SIL OFL 1.1 (`assets/fonts/JetBrainsMono-LICENSE.txt`) |
| Noto Sans Arabic, Devanagari, Thai and CJK SC, subset (`server/src/bench/fonts/`) | built into `lord-bench` for LordBench's shaping workload (subset by `tools/gen-lbfonts.py`) | SIL OFL 1.1 (`server/src/bench/fonts/OFL.txt`) |

### Local changes to bundled components

- **QuickJS** (`third_party/quickjs`, version in `VERSION`): adds `size_t JS_GetMallocSize(JSRuntime *rt)`
  (`quickjs.c`, next to `JS_ComputeMemoryUsage`, and its prototype in `quickjs.h`). It returns the
  same `malloc_size` that `JS_ComputeMemoryUsage` reports, without walking every object, so
  `lord.stats()` (`server/src/ui/js.c`) costs O(1). Re-apply it when QuickJS is updated.

## npm packages (bundled into the theme JavaScript at build time)

| Package | License |
|---|---|
| react, react-reconciler, scheduler | MIT |

## System libraries (Debian packages, dynamically linked)

These are not part of this repository; they are installed from Debian.

| Library | Linked by | License |
|---|---|---|
| libwayland-server / libwayland-client, libxkbcommon | lord-server | MIT |
| Mesa (libgbm, libEGL, libGLESv2) | lord-server (GPU path) | MIT |
| OpenSSL 3 | lord-sessiond, lord-maild | Apache-2.0 |
| libcrypt | lord-sessiond | LGPL-2.1+ |
| Poppler (glib), cairo, freetype, fontconfig | lord-doc | GPL-2.0-or-later / LGPL / FTL |
| libarchive | lord-doc | BSD-2-Clause |
| FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample), libva | lord-media | LGPL-2.1+; Debian's build is GPL-2.0-or-later |

Note: `lord-doc` and `lord-media` are separate helper processes. Because they link Poppler and
Debian's FFmpeg build, those two binaries as distributed are covered by the GPL; the MIT-licensed
LordOS source they are built from is GPL-compatible. `lord-server` itself does not link them.

## Tools used to make website media (not shipped in LordOS)

| Tool | Used for | License |
|---|---|---|
| Kokoro-82M (model and voices, run with kokoro-onnx) | the English narration of the LordOS Imager guide video on lordos.org/imager (voice `am_michael`) | Apache-2.0 |
