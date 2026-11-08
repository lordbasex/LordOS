# Voice: text to speech (0.8.8.6, in development)

LordOS reads text aloud on the computer, without the internet. Speech comes from **Pocket TTS** (Kyutai; code MIT,
model weights **CC-BY-4.0**: use, redistribution and commercial use allowed, crediting Kyutai), run by **audio.cpp**
(Apache-2.0) and its **ggml**, on the CPU or the GPU (Vulkan).

It is **off by default and no voice model is installed**: the person downloads the languages they want
(Settings → AI → Voice, phase 4). Models: Spanish, English and Portuguese, about 130 MB each.

## Phases

| Phase | What | State |
|---|---|---|
| 1 | `lord-tts` (the engine) and `say` (the command) | done |
| 2 | Read the selection anywhere: a global shortcut (primary selection), "Read aloud" in LordOS apps' menus | to do |
| 3 | Settings → AI → Voice: CPU / GPU / Auto, downloads with a progress bar, voices (record or upload, with consent) | to do |
| 4 | Models as signed LordOS packages (`lordos-voice-es`, …), a high-quality ("HD", 24-layer) model, LordOS's own voice | to do |

## `say`

```sh
say "Hola, ¿cómo estás?"          # the default language and voice
say -l en "Hello"                  # English's built-in voice (alba)
say -l es -v mi_voz "Hola"         # a voice to clone: ~/.local/share/lordos/voice/voices/mi_voz.wav
echo texto | say                   # stdin; terminal colors and control characters are dropped
say -f notas.txt -o notas.wav      # to a file
say -b cpu "…"                     # auto (the GPU when there is one), cpu or gpu
say -q hd "…"                      # standard or hd (falls back to the other when one isn't installed)
say --voices
```

Defaults: `~/.config/lordos/voice.json` (`language`, `voice`, `backend`, `quality`), then `$LANG`.

- **Models:** `<root>/models/<es|en|pt>/<standard|hd>.gguf`, root being `/usr/share/lordos/voice` (packages) or
  `~/.local/share/lordos/voice`.
- **Voices:** `<root>/voices/<name>.wav` (16-bit or float WAV, 0.5–30 s; the voice is cloned from it), or a voice the
  model carries (English: `alba`). Spanish and Portuguese carry none.
- **Speed:** a sentence at a time; each one plays (PipeWire's `pw-cat`) while the next is synthesized. Measured on an
  i9-10900 with an RX 580: about 10 s of speech in 1.3 s on the GPU and 3 s on the CPU, 220 MB of memory.
- `/usr/bin/say` is an alternative (`update-alternatives`), so another package's `say` can be installed too.

## lord-tts

`lord-say` holds no speech code: it starts **lord-tts** (next to it in `/usr/lib/lordos/bin`) on a `SOCK_SEQPACKET`
channel at fd 3. lord-tts is lord-doc's helper loop (`server/src/doc/helper.c`) built with `LORD_HELPER_TTS` and only
`src/doc/h_tts.c`; it is the **only** binary that links `libaudiocpp` and ggml, kept in `/usr/lib/lordos/lib/tts`
(ggml loads its CPU kernels for the running processor, and Vulkan, from there).

Protocol (`src/doc/proto.h`): `DQ_INIT_FD` with the models folder (role `ro`) and `~/.cache/lordos/voice` (role `rw`),
`DQ_INIT_GO {"mode":"tts","gpu":bool}`, then `DQ_TTS {"slot","file","text","backend","threads","voice_id","seed"}`
plus, optionally, the voice WAV's fd → `DR_BLOB`: a 16-bit mono WAV in a sealed memfd. The model is loaded by the
first request and kept; another file or backend is refused.

**Its sandbox** (`sandbox_apply_helper`, scope `lordos-tts`), checked by `tests/tts_test.sh`:

- reads `/usr` and its own library folder; reads the models folder it was given; writes only `~/.cache/lordos/voice`
  (audio.cpp unpacks a model's tokenizer in `tmp/`, Mesa keeps compiled shaders in `gpu/`);
- no network, no sockets, no fork, no exec; threads only; memory capped by its cgroup (4 GB);
- **on the GPU only** (`gpu: true`): opens `/dev/dri/renderD*` (the render node), DRM ioctls only, lists `/dev/dri`
  without opening anything else in it (`list_dirs`, Landlock `READ_DIR`), reads the parts of `/sys` the Vulkan loader
  and Mesa use to find the card. Never `/dev/dri/card0`, the display node lord-server drives;
- a voice file is untrusted data: the WAV reader checks every size against the file (`read_voice`).

## Building

`sh tools/tts/fetch-audiocpp.sh` puts audio.cpp at the commit LordOS was tested with in `third_party/audiocpp` (ignored
by git; the image builders run it). `server/mk/tts.mk` builds its Pocket TTS family only, with
`-DENGINE_ENABLE_CPU_ALL_VARIANTS=ON` (so one image serves every x86-64 processor) and Vulkan. It needs `cmake`,
`glslc`, `libvulkan-dev` and `spirv-headers`; without them `make` skips lord-tts.

Tests: `server/tests/saytext_test.c` (cleaning terminal output, splitting sentences; `make test`) and
`server/tests/tts_test.sh` (speech in es/en/pt on the CPU and the GPU, the confinement in both, a voice file that
isn't audio, a voice name that is a path; `make test-full` when a model is installed).

Not yet tested: the Raspberry Pi (aarch64: ggml's CPU variants and the Pi 5's Vulkan driver).

## Credits

Pocket TTS © Kyutai, CC-BY-4.0 (https://huggingface.co/kyutai/pocket-tts), converted to GGUF by audio.cpp. Kyutai
forbids cloning a voice without the person's explicit consent, impersonation and presenting generated speech as a real
recording; LordOS shows that notice where voices are created.
