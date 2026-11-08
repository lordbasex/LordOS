# Voice: text to speech (0.8.8.6, in development)

LordOS reads text aloud on the computer, without the internet. Speech comes from **Pocket TTS** (Kyutai; code MIT,
model weights **CC-BY-4.0**: use, redistribution and commercial use allowed, crediting Kyutai), run by **audio.cpp**
(Apache-2.0) and its **ggml**, on the CPU or the GPU (Vulkan).

**No voice model is installed by default**: the person downloads the languages, qualities and voices they want from
Settings → AI → Voice, as signed LordOS packages (below). Reading aloud from the desktop is on by default and can be
turned off there; without a model it says so.

## Phases

| Phase | What | State |
|---|---|---|
| 1 | `lord-tts` (the engine) and `say` (the command) | done |
| 2 | Read the selection anywhere: Edit → Speech and the right-click menu, ❖Esc, the player, saving as a sound file | done |
| 3 | Settings → AI → Voice: models (standard and HD) and 23 voices per language as signed packages, heard before downloading; Personal Voice (read six phrases or speak freely); Live Speech | done |
| 4 | LordOS's own voice; more languages | to do |

## `say`

```sh
say "Hola, ¿cómo estás?"          # the language's chosen voice (else its own: Lola, Rafael, Alba)
say -v juergen "Hola"              # an installed voice
say -l es -v mi_voz "Hola"         # a voice to clone: ~/.local/share/lordos/voice/voices/mi_voz.wav
echo texto | say                   # stdin; terminal colors and control characters are dropped
say -f notas.txt -o notas.wav      # to a file
say -b cpu "…"                     # auto (the GPU when there is one), cpu or gpu
say -q hd "…"                      # auto (HD when installed), standard or hd; falls back to the other
say --voices                       # every installed voice, by language and quality, and the voices to clone
say --preview es lola              # a voice's recorded sample (lordos-voice-previews)
```

Defaults: `~/.config/lordos/voice.json` (`voices` per language, `voice`, `language`, `backend`, `quality`), then
`$LANG`.

- **Models**, root being `/usr/share/lordos/voice` (packages) or `~/.local/share/lordos/voice`: standard
  `<root>/models/<lang>/standard.gguf`, HD `<root>/models/<lang>/hd/` (Kyutai's `model.safetensors`, `tokenizer.model`).
- **Voices:** `<root>/models/<lang>/[hd/]embeddings/<voice>.safetensors` (Kyutai's prepared voices), or
  `<root>/voices/<name>.wav` (16-bit or float WAV, 0.5–30 s; the voice is cloned from it). A voice asked for that isn't
  installed for the model falls back to the language's own, then to any installed one.
- **Speed:** a sentence at a time; each one plays (PipeWire's `pw-cat`) while the next is synthesized. Measured on an
  i9-10900 with an RX 580: about 10 s of speech in 1.3 s on the GPU and 3 s on the CPU, 220 MB of memory.
- `/usr/bin/say` is an alternative (`update-alternatives`), so another package's `say` can be installed too.
- **Status note** (0.8.8.6): once the first sentence is made, `say` writes `~/.cache/lordos/say/<pid>` with
  `lord-say 1 <vulkan|cpu> <hd|standard> <language> <speak|save>` and removes it when it ends (also on SIGINT and
  SIGTERM; notes of `say` processes that are gone are removed by the next one). Monitor → GPU reads it to say what the
  voice runs on (`docs/MONITOR.md`, "GPU"): lord-tts itself is sandboxed and not dumpable, so its `/proc` isn't
  readable. It is not lord-tts's folder: the helper can't write it.

## Reading aloud from the desktop

Every place with text has **Speech** (`sdk/voice.js`, `speechMenu`):

- **Edit → Speech** (Text Editor, Terminal; Notes in View) and the **right-click** menu (Text Editor, Terminal, Notes):
  *Start Speaking*, *Stop Speaking*, *Save as Audio…* Start Speaking reads the selection, or the whole text when
  nothing is selected (Text Editor, Notes). The right-click menu is the SDK's (`sdk/contextmenu.jsx`, with submenus).
- **A Linux app** (Firefox, foot…) gets an **Edit** menu with Speech in the menu bar (Lord Classic: in its window's
  menu strip), as its own menus aren't the desktop's (`useMenusWithSpeech`). It reads the app's **primary selection**.
- **❖Esc** reads what the window in front has selected; pressed again while reading, it stops (`sdk/shortcuts.js`).
- **The player** (`sdk/voiceplayer.jsx`): a small pill with the voice's wave, ↻ read again, ⏸/▶ pause and go on,
  ■ stop (it stays, to read again), ✕ stop and close. Drag it by its grip anywhere, to any display: the next reading
  opens there (`settings.voice.pos`: the display's name and the place in it; a display that isn't there any more falls
  back to the main one; `sdk/voiceplayermodel.js`). It goes a few seconds after the end.
- **The wave** is real: `lord-say --levels` measures each 40 ms of audio it plays in 16 bands, 80 Hz to 8 kHz
  (`src/tts/levels.c`), and writes them to lord-server, which shows the one being heard (`speak_levels`: the time since
  the first, less the pauses; it waits when the next sentence isn't ready). In a pause the wave stands still.
- **Save as Audio…**: the save dialog, in the Music folder, the name
  from the first words; a 16-bit mono WAV, nothing is heard.
- **Settings → AI → Voice** (`apps/settings/AI.jsx`): *Read aloud* (Speech and ❖Esc go away without it; `say` keeps
  working), *Keyboard shortcut ❖Esc*, *Show the player*, all on by default (`settings.voice`), and *Process with*
  Automatic / Processor / Graphics card (`voice.json`'s `backend`, which `say` reads too).

**How lord-server does it** (`src/tts/speak.c`, `src/ui/js_voice.c`, `lord.voice`):

- It starts `lord-say` from the **fixed path next to its own executable**, owned by root or the user and writable by
  no one else, with `posix_spawn`. Never the text in the arguments: the text is stdin, a **sealed memfd**, so the
  compositor never blocks writing it. Arguments are only `--levels` (reading) or `-o -` (saving). Every fd of
  lord-server's is closed (`addclosefrom_np`): the child gets stdin, /dev/null or the file, and the levels pipe at
  fd 3. A minimal environment: HOME, XDG_RUNTIME_DIR (PipeWire), LANG (the interface's language, for a text too short
  to tell) and PATH=/usr/bin:/bin.
- One at a time, in its own process group: stop sends SIGTERM to the group (lord-say ends lord-tts; pw-cat with it);
  a pause is SIGSTOP to the group, so the sound stops at once, and SIGCONT goes on. Children are reaped there, never by
  a global `waitpid`.
- **A Linux app's selection never reaches JS.** The compositor keeps who made the primary selection (`prim_pid`) and
  `wl_focus_primary` gives it only when that client has the keyboard. `lord.voice.speakSelection()` reads it there;
  for saving, `holdSelection()` keeps a copy in C while the dialog has the keyboard and `saveSelection(path)` uses
  and wipes it.
- **Saving:** the path is checked as `lord.fs.write`'s (`js_sys_resolve_target`: the home folder or a mount, not a
  protected folder) and must end in `.wav`. lord-server opens the folder (`O_NOFOLLOW`), creates a hidden file in it
  with `O_EXCL` and gives it to lord-say as stdout; when lord-say ends well the file is renamed in that same folder
  (`renameat`), else removed. lord-say never sees a path.
- Isolated `.lpk` apps have no `lord.voice` (the broker doesn't offer it).

## Models, voices and packages (phase 3)

Two qualities per language, both Kyutai's Pocket TTS:

| Quality | What | Size (es / pt / en) |
|---|---|---|
| Standard | the 6-layer model, audio.cpp's Q8_0 GGUF: fast anywhere | 128 MB each |
| HD | Kyutai's 24-layer model (`spanish_24l`, `portuguese_24l`, `english_2026-09_24l`), read as Kyutai ships it: more natural | 672 / 672 / 1304 MB |

**23 voices** (`sdk/voicecatalog.js`), each speaking every language: Kyutai's prepared voice states (21–31 MB in HD,
4–8 MB standard). Only voices whose recording allows redistribution and commercial use: Kyutai's own (lola, juergen,
rafael, estelle, giovanni, daan), voice-zero (CC0), Alba McKenna and VCTK (CC-BY-4.0). Left out: jean (EARS,
non-commercial) and cosette, javert, marius (origin not documented). Daan exists only in HD. Each language's own voice
is the default: Lola (es), Rafael (pt), Alba (en).

**Packages** (`tools/voice/mkdeb-voice.mjs`, `Architecture: all`, into `/usr/share/lordos/voice`), published in the
signed APT repository, never fetched from Hugging Face by a LordOS system:

- `lordos-voice-<lang>` and `lordos-voice-<lang>-hd`: the models;
- `lordos-voice-<lang>-<voice>` and `…-hd`: one voice, depending on its model;
- `lordos-voice-previews`: a recorded sample of every voice in every language (5 MB), so a voice is heard before it is
  downloaded (`say --preview`, `lord.voice.preview`).

Building them, on the maintainer's machine: `tools/voice/fetch-voices.sh <dir>` (Kyutai's gated repository needs its
conditions accepted and a read token; each file checked against the SHA-256 Hugging Face publishes), then
`tools/voice/make-previews.sh <dir>`, then `node tools/voice/mkdeb-voice.mjs <dir> <out> <YYYY.MM.DD>`. The packages
are reproducible; `tools/release/apt-repo.sh build … <the .debs>` publishes them like any other.

**What lord-tts does like Kyutai's own code** (`pocket_tts`, checked against it): temperature 0.3 for every model
(audio.cpp defaults to 0.7 outside English, which made Spanish drag and stumble), and the model's character
replacements before synthesis (quotes, brackets and, in Spanish, `¡ ¿` dropped; curly apostrophes to `'`; a dash to a
comma). The model starts every text with 0.3–0.9 s of silence (Kyutai's code too); `say` cuts it from the first
sentence, keeping 50 ms.

## Settings → AI → Voice

`apps/settings/AI.jsx`: Read aloud, ❖Esc, the player, Live Speech, Process with (Automatic / Processor / Graphics card)
and Quality (Automatic: HD when installed / Standard / HD), then per language the two models (Download with a progress
bar, Remove) and the voices: ▶ plays the recorded sample, Download, Try (reads the language's sample sentence), Use,
Remove. `say` reads the choice from `~/.config/lordos/voice.json`: `{ "voices": { "es": "juergen" }, "quality",
"backend" }`. Installing and removing go through `admin.run('pkg-install' | 'pkg-remove')` (an administrator), with
only `lordos-voice-*` names (`sdk/voicemanager.js`).

**Personal Voice** (`apps/settings/PersonalVoice.jsx`): the person's own voice, cloned by the model from a short
recording, ready at once and usable in every language.

1. What it is and the consent, required before anything is recorded (Kyutai's terms forbid cloning without it).
2. Two ways, both offered: **read six short phrases**, one at a time in large type (each stops by itself when it was
   read), or **speak freely** for 15–30 s about anything, with a few ideas shown, for anyone reading aloud is hard for.
3. Three seconds of the room first: too noisy says so (the voice would copy the noise); a microphone that sends nothing
   says so too.
4. The recordings (lord-media, the microphone permission) are trimmed of silence and joined into one 16-bit mono WAV of
   at most 30 s (`sdk/voicerecmodel.js`), saved as `~/.local/share/lordos/voice/voices/<name>.wav` and chosen for the
   language. It never leaves the computer. **Use a recording…** does the same from an audio file (WAV, MP3, M4A, OGG,
   Opus or FLAC; at least 3 s; the first 30 s, converted by lord-media).

**Live Speech** (`sdk/livespeech.jsx`, ❖⇧Esc): a box to type in; Enter says it with the chosen voice, the last
phrases stay below to be said again (in memory only). Esc stops, then closes.

## lord-tts

`lord-say` holds no speech code: it starts **lord-tts** (next to it in `/usr/lib/lordos/bin`) on a `SOCK_SEQPACKET`
channel at fd 3. lord-tts is lord-doc's helper loop (`server/src/doc/helper.c`) built with `LORD_HELPER_TTS` and only
`src/doc/h_tts.c`; it is the **only** binary that links `libaudiocpp` and ggml, kept in `/usr/lib/lordos/lib/tts`
(ggml loads its CPU kernels for the running processor, and Vulkan, from there).

Protocol (`src/doc/proto.h`): `DQ_INIT_FD` with the models folder (role `ro`) and `~/.cache/lordos/voice` (role `rw`),
`DQ_INIT_GO {"mode":"tts","gpu":bool}`, then `DQ_TTS {"slot","file","text","backend","threads","voice_id","seed","language"}`
plus, optionally, the voice WAV's fd → `DR_BLOB`: a 16-bit mono WAV in a sealed memfd. The model is loaded by the
first request and kept; another file, backend or language is refused. With `language` (only `spanish_24l`,
`portuguese_24l`, `english_2026-09_24l`), `file` is an HD model's folder, read with `lib/tts/pocket_tts_dir.json`
(audio.cpp's own description of the model, generated at build time to read that folder).

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

Tests: `server/tests/saytext_test.c` (cleaning terminal output, splitting sentences, telling the language),
`server/tests/levels_test.c` (the wave's bands), `server/tests/voiceplayer_model_test.mjs` (where the player goes, the
wave's motion; all three in `make test`) and
`server/tests/tts_test.sh` (speech in es/en/pt on the CPU and the GPU, the confinement in both, a voice file that
isn't audio, a voice name that is a path; `make test-full` when a model is installed).

Not yet tested: the Raspberry Pi (aarch64: ggml's CPU variants and the Pi 5's Vulkan driver).

## Credits

Pocket TTS © Kyutai, CC-BY-4.0 (https://huggingface.co/kyutai/pocket-tts), converted to GGUF by audio.cpp. Kyutai
forbids cloning a voice without the person's explicit consent, impersonation and presenting generated speech as a real
recording; LordOS shows that notice where voices are created.
