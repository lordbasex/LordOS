# Third-party components

LordOS's own code is released under the MIT License (see `LICENSE`). It bundles or links
the components below, each under its own license.

## Bundled in this repository (`third_party/`)

| Component | Used for | License |
|---|---|---|
| QuickJS | JavaScript engine for the UI | MIT (`third_party/quickjs/LICENSE`) |
| stb_image, stb_image_write, stb_truetype | image decoding/encoding, font rasterization | MIT or public domain (dual) |
| Monocypher 4 | vault and keyring cryptography | CC0-1.0 or BSD-2-Clause (`third_party/monocypher/LICENCE.md`) |
| Wayland's `wl_fixes` interface (wayland.xml 1.24) | the core `wl_fixes` global on a libwayland older than 1.24 (`server/mk/wlfixes.mk`) | MIT (in `third_party/wayland/wl-fixes.xml`) |
| EFF large wordlist, SecLists common passwords | passphrase generator, weak-password check | CC BY 3.0 (EFF), MIT (SecLists, `third_party/wordlists/LICENSE-SecLists`) |
| `pointer-warp-v1.xml` from wayland-protocols 1.45 (`staging/pointer-warp`, commit `b3f29d8a41320a9231587835ee5e485f4a5f6eba`; Debian 13 ships 1.44 without it), in `server/protocols/`, unmodified | `wp_pointer_warp_v1` (wayland-scanner generates the code) | MIT (copyright notice in the file) |
| wayland-protocols 1.50: `ext-background-effect-v1.xml`, `xdg-session-management-v1.xml` (in `server/protocols/`, unchanged; newer than Debian 13's 1.44) | protocol descriptions wayland-scanner turns into code for lord-server | MIT (the notice is in each file) |
| KDE's `server-decoration.xml` (plasma-wayland-protocols, as shipped in the KDE Platform 6.11 runtime; SHA-256 `c5f857734bb190dc9adfb65fb05550a6245f12af1a2be5eefd05be199805a68c`), in `third_party/plasma-wayland-protocols/`, unmodified | only the Wayland conformance tool's test client (`tools/wl-conformance.mjs` runs wayland-scanner on it); lord-server's `org_kde_kwin_server_decoration_manager` stays hand-written in `wl.c` | LGPL-2.1-or-later (`third_party/plasma-wayland-protocols/LGPL-2.1.txt`; the notice is in the file) |

## Fetched at build time (`third_party/audiocpp`, ignored by git)

`tools/tts/fetch-audiocpp.sh` puts audio.cpp at one pinned commit in `third_party/audiocpp`; `server/mk/tts.mk` builds
its Pocket TTS family only, and the `lordos` package installs the libraries in `/usr/lib/lordos/lib/tts`. **lord-tts**
(0.8.8.6, `docs/VOICE.md`) is the only binary that links them; `lord-server` never does. Unmodified.

| Component (version) | Used for | License |
|---|---|---|
| audio.cpp (commit `3bbbd1d28eb8a88b202907dc102595e762340b1c`, https://github.com/0xShug0/audio.cpp) | the text-to-speech runtime (`libaudiocpp`) | Apache-2.0 (`third_party/audiocpp/LICENSE`) |
| ggml 0.12.0, as vendored by audio.cpp (`external/ggml`) | the tensor library under audio.cpp: `libggml`, `libggml-base`, the CPU kernels of every x86-64 generation, Vulkan | MIT (`external/ggml/LICENSE`) |
| SentencePiece 0.2.1, with its Abseil, protobuf-lite and darts-clone (`external/sentencepiece`) | the model's tokenizer, linked into `libaudiocpp` | Apache-2.0 (SentencePiece, Abseil), BSD-3-Clause (protobuf-lite, darts-clone) |
| cJSON 1.7.19, libyaml 0.2.5 (`external/cJSON`, `external/libyaml`) | reading audio.cpp's model descriptions, linked into `libaudiocpp` | MIT |

## Fetched at build time for the GVT-g option ROM (in development)

`tools/firmware/build-gvt-gop.sh` downloads EDK2 at one pinned tag (tarball SHA-256 and embedded commit checked) into
a work folder outside the repository and builds LordOS's own GVT-g GOP driver (`firmware/gvt-gop`, MIT, sources in
its `NOTICE.md`) against it. The option ROM links EDK2 library code; nothing else in LordOS does. Unmodified.

| Component (version) | Used for | License |
|---|---|---|
| EDK2 `edk2-stable202502` (commit `fbe0805b2091393406952e84724188f8c1941837`, https://github.com/tianocore/edk2) | BaseTools (GenFw, EfiRom) and the MdePkg/MdeModulePkg libraries linked into the ROM (BaseLib, UefiLib, FrameBufferBltLib…); OvmfPkg's debug-port library in DEBUG builds | BSD-2-Clause-Patent |

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
| MediaPipe Selfie Segmenter (landscape) and BlazeFace short range, by Google (`third_party/studio-models/`: the original `.tflite` files and Google's model cards; converted by `tools/studio/tflite2lnn.mjs` into `assets/studio/models/*.lnn`, which LordOS ships) | LordOS Studio's person segmentation (Portrait, Background) and, in later phases, face finding; run by LordOS's own inference code in `lord-studio` (`server/src/media/studio/nn.c`), no ML library | Apache-2.0 (`third_party/studio-models/LICENSE`; sources, hashes and the modification notice in `third_party/studio-models/README.md`) |
| LordOS Studio's test frames (`server/tests/data/studio/`): ten photographs from Wikimedia Commons, cropped and scaled | `build/studio-nn-test`, `build/studio-test` (not shipped) | CC0 1.0 (authors and pages in `server/tests/data/studio/README.md`) |

### Local changes to bundled components

Each change is marked `LordOS patch` in the source and has to be applied again when the component is
updated. Nothing else in `third_party/` differs from upstream.

| Component (upstream version) | Change | Files | Why | Since |
|---|---|---|---|---|
| QuickJS 2025-04-26 (`third_party/quickjs/VERSION`) | Adds `size_t JS_GetMallocSize(JSRuntime *rt)`, which returns `rt->malloc_state.malloc_size`: the same `malloc_size` that `JS_ComputeMemoryUsage` reports, without walking every object, string and shape of the runtime. No existing function or structure changes. | `quickjs.c` (7 lines, just before `JS_ComputeMemoryUsage`), `quickjs.h` (its prototype, after `JS_ComputeMemoryUsage`'s) | `lord.stats()` (`f_stats` in `server/src/ui/js.c`) gives `jsKB` from it in O(1); About, Settings → General and System Information poll it while open, and the full walk grew with the number of objects. Checked equal to `JS_ComputeMemoryUsage`'s `malloc_size` on a runtime with 100,000 objects, a 50,000-entry Map and after GC | 0.8.6 |

## Moving wallpapers

Downloaded on demand, not in this repository; each clip's author, license and source are in Settings → Wallpaper → Credits
and in the README of the `LordOS-wallpapers` repository. NASA material (public domain), CC0, CC BY 3.0 and CC BY-SA 4.0
works; see `docs/WALLPAPERS.md`. The built-in clip is the project's own.

## LordOS Studio's backgrounds

The catalog `assets/studio/backgrounds.json` lists 158 backgrounds (121 pictures, 37 short loops) with each one's title,
author, license, license URL, source page and, for CC BY and CC BY-SA works, its credit line; the camera popover shows
them under Background → Credits. Their thumbnails and seven pictures (one per category) ship in
`assets/studio/backgrounds/`; the full files are downloaded on demand from the `LordOS-wallpapers` repository's
`studio/` folder, whose README credits every file.

The 121 pictures (72 backgrounds and 49 stills of moving scenes) were generated with ChatGPT for LordOS from the
project's own prompts on 2026-11-22 and approved by the maintainer the same day. OpenAI's terms assign the output to
the user; the LordOS project releases them under CC0 1.0 (`"ai": true` in the catalog; the credits page says
"Generated with AI"). No photograph, person, logo or readable text was used or asked for. They are made by
`tools/studio/build-ai-backgrounds.py`.

Every loop is published at 3840×2160 and 1920×1080, made from an original at least 3840×2160. The loops come from
Wikimedia Commons (CC0, CC BY 2.0/3.0/4.0 and CC BY-SA 4.0; the share-alike applies to those files only) and the
project's own renders (MIT, `tools/studio/gen-studio-loops.py`). The record of each loop's license check is
`tools/studio/bg-sources.json`. Nothing from Unsplash, Pexels or Pixabay (`docs/CAMERA-STUDIO-PLAN.md` 6.1).

LordOS Studio's Noise Reduction uses PipeWire's own echo-cancel module with WebRTC Audio Processing (BSD-3-Clause), both
Debian packages LordOS already installs (`libspa-0.2-modules`, `libwebrtc-audio-processing-1-3`); LordOS ships only the
configuration file that loads them.

## Voices

Downloaded on demand from the signed APT repository (Settings → AI → Voice), not in this repository and not on the ISO
or the Raspberry Pi image: the packages `lordos-voice-<lang>[-hd]` (the models), `lordos-voice-<lang>-<voice>[-hd]`
(one voice each) and `lordos-voice-previews` (a recorded sample of every voice), built by `tools/voice/mkdeb-voice.mjs`
into `/usr/share/lordos/voice`. The voices offered and the license of each one's source are in `sdk/voicecatalog.js`
(`VOICES`, `SOURCE_LICENSES`); see `docs/VOICE.md`.

| Data | Used for | License |
|---|---|---|
| Pocket TTS models by Kyutai (https://huggingface.co/kyutai/pocket-tts): the 6-layer models as audio.cpp's Q8_0 GGUF (Dutch's as Kyutai ships it, `dutch`: there is no GGUF of it), the 24-layer HD models (`spanish_24l`, `portuguese_24l`, `english_2026-09_24l`, `french_24l`, `german_24l`, `italian_24l`, `dutch_24l`) as Kyutai ships them | speech in Spanish, English and Portuguese; French, German, Italian and Dutch in development | CC-BY-4.0 — Pocket TTS © Kyutai |
| Kyutai's prepared voice embeddings (https://huggingface.co/kyutai/tts-voices): Kyutai's own voices (lola, juergen, rafael, estelle, giovanni, daan), Alba McKenna, VCTK and voice-zero | the 23 voices | CC-BY-4.0 (Kyutai, Alba McKenna, VCTK); CC0-1.0 (voice-zero) |
| The recorded previews (`lordos-voice-previews`) | hearing a voice before downloading it | made with the models and voices above, under their licenses |

Kyutai forbids cloning a voice without the person's explicit consent, impersonation and presenting generated speech as
a real recording; LordOS shows that notice where voices are created. Voices whose recording does not allow
redistribution and commercial use are left out (jean, from EARS: non-commercial only; cosette, javert and marius: their
recordings' origin isn't documented).

## Community themes

| Theme | Credit | License |
|---|---|---|
| Celeste (`themes/celeste`) | Layout inspired by Caelestia (caelestia-dots/shell, GPL-3.0); no code or assets from it. Author: Federico Pereira (IPERFEX); kept in LordOS-themes since 0.8.9.6.2 | MIT |

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
| libsrtp2, usrsctp | lord-sessiond (the remote desktop's WebRTC video, 0.8.4) | BSD-3-Clause |
| libcrypt | lord-sessiond | LGPL-2.1+ |
| Poppler (glib), cairo, freetype, fontconfig | lord-doc | GPL-2.0-or-later / LGPL / FTL |
| libarchive | lord-doc | BSD-2-Clause |
| FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample), libva | lord-media, lord-stream and lord-studio (LordOS Studio's camera, which also links Mesa's libgbm, libEGL and libGLESv2 like lord-bench) | LGPL-2.1+; Debian's build is GPL-2.0-or-later |
| Tesseract (libtesseract5) and its English, Spanish, Portuguese, French and Italian data (`tesseract-ocr-*`) | lord-ocr (0.8.8.6: Screenshots' copy the text, the markup editor's Transcribe) | Apache-2.0 |
| Vulkan loader (libvulkan1), OpenMP (libgomp1), the C++ runtime (libstdc++6) | lord-tts (0.8.8.6) | Apache-2.0 (Vulkan loader); GPL-3.0 with the GCC Runtime Library Exception |

Note: `lord-doc` and `lord-media` (and `lord-stream` and `lord-studio`, built from its sources) are separate helper processes. Because they link Poppler and
Debian's FFmpeg build, those two binaries as distributed are covered by the GPL; the MIT-licensed
LordOS source they are built from is GPL-compatible. `lord-server` itself does not link them.
`lord-tts` links audio.cpp and ggml (above) in the same way: a separate sandboxed helper.
On the Raspberry Pi image, and on a Pi updated to 0.8.7, `lord-media` runs on Raspberry Pi's build of FFmpeg (`+rpt`, the
same package names), pinned from its archive for the Pi 5's hardware HEVC (`docs/WALLPAPERS.md`).

These come with `lordos` for Files and the remote apps (0.8.7): **FreeRDP** (`libfreerdp3-3`, `libfreerdp-client3-3`, `libwinpr3-3`, Apache-2.0), **rclone** (MIT; since 0.8.8.5 rclone's official release binary v1.75.2, unmodified, in the package `lordos-rclone`, for Cloud and Connect to Server: its signed `SHA256SUMS` is checked against Nick Craig-Wood's key `FBF737ECE9F8AB18604BD2AC93935E02FF3B54FA`; `/usr/share/doc/lordos-rclone/copyright` lists the license of every Go module linked in), **fuse3** (LGPL-2.1, GPL-2.0) and openssh-client (BSD). The Store's Disk Formats item, not preinstalled, brings **ntfs-3g** (GPL-2.0), **exfatprogs** (GPL-2.0), dosfstools (GPL-3.0) and, on amd64, **apfs-dkms** (GPL-2.0).

Machines (0.8.8.5) ships no virtualization engine: when it is first used it asks to install, from Debian, **QEMU** (`qemu-system-x86` or `qemu-system-arm`, `qemu-system-modules-opengl`, `qemu-utils`; GPL-2.0), **passt** (GPL-2.0-or-later) and the UEFI firmware **OVMF** or **AAVMF** (`ovmf`, `qemu-efi-aarch64`; BSD-2-Clause-Patent). `lord-vm` and `lord-vmdisplay` run them as separate programs and link none of them.

New on the images in 0.8.8 (installed from Debian, not linked; each keeps its own `/usr/share/doc/<package>/copyright`):

| Package | Images | What for | License |
|---|---|---|---|
| `polkitd`, `pkexec` | both ISOs and the Raspberry Pi image; dependencies of `lordos` | administrator rights for Linux apps that run `pkexec`, answered by LordOS's own prompt (`docs/PRIVACY.md`) | LGPL-2.0-or-later |
| `libnss-mdns` | both ISOs and the Raspberry Pi image; recommended by `lordos` | `.local` names | LGPL-2.1-or-later |
| `wireless-regdb` | both ISOs and the Raspberry Pi image | Wi-Fi's country rules for the kernel | ISC |
| `firmware-brcm80211` | both ISOs (the Raspberry Pi image already had it, from Raspberry Pi's archive) | Broadcom Wi-Fi cards such as the BCM43602 | **non-free firmware** (Debian's `non-free-firmware`): Broadcom's binary licence, which allows redistribution unmodified; no source |
| `firmware-mediatek`, `firmware-intel-graphics` | both ISOs (in development, after 0.8.9.5-beta) | MediaTek Wi-Fi and Bluetooth such as the MT7921/MT7922 (#227); Intel graphics firmware (i915 DMC/GuC/HuC, xe) | **non-free firmware** (Debian's `non-free-firmware`): the vendors' binary licences, which allow redistribution unmodified; no source |

New on the images in 0.8.8.6 (installed from Debian, not linked):

| Package | Images | What for | License |
|---|---|---|---|
| `libtesseract5`, `tesseract-ocr-eng`, `-spa`, `-por`, `-fra`, `-ita` | both ISOs and the Raspberry Pi image; `libtesseract5` and English are dependencies of `lordos` when it ships `lord-ocr`, the other four are Recommends | text recognition (lord-ocr, above) | Apache-2.0 |
| `intel-microcode`, `amd64-microcode` | the amd64 ISO; a dependency of `lordos` on amd64 | the processor's microcode updates, loaded at boot | **non-free firmware** (Debian's `non-free-firmware`): Intel's and AMD's binary licences, which allow redistribution unmodified; no source |

## Tools used to make website media (not shipped in LordOS)

| Tool | Used for | License |
|---|---|---|
| Kokoro-82M (model and voices, run with kokoro-onnx) | the English narration of the LordOS Imager guide video on lordos.org/imager (voice `am_michael`) | Apache-2.0 |
