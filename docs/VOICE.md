# Voice: text to speech (0.8.8.6, in development)

LordOS reads text aloud on the computer, without the internet. Speech comes from **Pocket TTS** (Kyutai; code MIT,
model weights **CC-BY-4.0**: use, redistribution and commercial use allowed, crediting Kyutai), run by **audio.cpp**
(Apache-2.0) and its **ggml**, on the CPU or the GPU (Vulkan).

**No voice model is installed by default**: the person downloads the languages they want (Settings → AI → Voice,
phase 4). Models: Spanish, English and Portuguese, about 130 MB each. Reading aloud from the desktop (below) is on by
default and can be turned off in Settings → AI; without a model it says so.

## Phases

| Phase | What | State |
|---|---|---|
| 1 | `lord-tts` (the engine) and `say` (the command) | done |
| 2 | Read the selection anywhere, as macOS: Edit → Speech and the right-click menu, ❖Esc, the player, saving as a sound file | done |
| 3 | Settings → AI → Voice: on/off, ❖Esc, the player and CPU / GPU / Auto (done); downloads with a progress bar, voices (record or upload, with consent) | started |
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

## Reading aloud from the desktop

As on a Mac, every place with text has **Speech** (`sdk/voice.js`, `speechMenu`):

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
- **Save as Audio…** (as macOS's *Add to Music as a Spoken Track*): the save dialog, in the Music folder, the name
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
