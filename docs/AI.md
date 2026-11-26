# Settings → Artificial intelligence (0.8.9.6.2, in development)

The AI page of Settings is a store around **Lumo**, built like the wallpaper gallery (cards, filters with counts,
capability pages with a breadcrumb, a sheet per model). Code: `apps/settings/AI.jsx`; the pure part, tested by
`server/tests/ai_store_test.mjs`, is `sdk/aistore.js`. The approved mockup is the maintainer's (2026-11-25).

## Lumo and its pieces

At the top the 3D Lumo (`sdk/mascot.js`) and the chain of its five pieces, each ✓ when installed:

| Piece | Capability | Today |
|---|---|---|
| Ear | speech to text (Whisper, whisper.cpp) | **coming soon**: not inside LordOS yet |
| Brain | the language model (`lordos-llm-*` + the llama.cpp engine, `sdk/llmcatalog.js`) | ✓ with a model and the engine installed |
| Voice | reading aloud (`lordos-voice-*`, [VOICE.md](VOICE.md)) | ✓ with a language's model installed |
| Eyes | text recognition (Tesseract in the confined `lord-ocr` helper) | ✓ with Tesseract's English data |
| Hands | doing things in LordOS with your permission: the agent's tools ([HANDS.md](HANDS.md): `lordos-mcp`, `org.lordos.Hands1`) | ✓ part of LordOS; on only when the person turns it on for a session |

Lumo waves when every piece that exists today is installed and sleeps while one you could download is missing.
**Talk to Lumo** stays disabled, with a "coming soon" line, until the Ear exists (Lumo's own agent loop comes later and uses
the Hands' tools).

## One brain, many functions

The language model is one brain; "What Lumo can do" lists its functions as groups of LordOS tools (an MCP server
later), each with its level: **read only**, **with your permission** or **administrator** (the password always in the
system's dialog). Notes, Programs, Log audit, Sentinela, Files, Apps and windows, Machines, Settings and System. Only
**Sentinela** works today: its card shows whether it is on (`settings.sentinela.on`) and opens its page
(`apps/settings/Sentinela.jsx`, [SENTINELA.md](SENTINELA.md)); the others are marked "coming soon" (no toggles that
do nothing).

## Writing: ✨ AI on selected text (in development)

Approved by the maintainer on 2026-11-26. With text selected in a LordOS text field (`TextInput`, `TextArea`, Notes,
Mail's body), the right-click menu shows **✨ AI ▸ Correct spelling and grammar · Improve writing · Make it shorter ·
Translate to** (the interface language, the spelling languages, then English, Spanish, Portuguese, French, German,
Italian, Chinese, Japanese). Its function card is **Writing** in "What Lumo can do" (read only), with its own page
(`{ section: 'ai', page: 'writing' }`): **on whenever a model is installed** (on a system that has one and right after
installing one); the person can turn it off there, and that choice is kept (`settings.writing.on === false` is never
turned back on, a new model included; `sdk/writingmodel.js` `writingEnabled`: a default that depends on a model being
present, not a migration). Without a model it is off and the menu shows nothing. It uses the model chosen for Lumo,
else the first installed.

- **It runs in the shell, never in the app.** `lord.writing` (`server/src/ui/js_sentinela.c`) exists only in the
  desktop's own JS context; an isolated app's broker has none. The SDK's field hands the selection to the shell's
  panel (`sdk/writing.jsx`); the app gets text back only through its field's change after the person clicks Replace.
- **Fixed prompts.** `server/src/llm/writing.c` holds every text the model gets; JS sends only the action name, a
  target of the closed list of LordOS's 35 interface languages and the text. A short system text says the text is
  data and never instructions; each user message holds the task ("Proofread this text…", "Translate this text into
  Portuguese…") and then the text between `<<<TEXT nonce>>>` and `<<<END-TEXT nonce>>>` with a random nonce; one or
  two fixed worked examples go first as earlier turns of the conversation (a Spanish and an English correction; for a
  translation, each target language's own: an informal chat line without accents, and a sentence with a request in
  it, translated as text). The first version (one long system text, the user message only the fenced text) made the
  1.7B model copy the text back for every action, translation included (the maintainer's test on ws164, measured
  again with `build/writing-real-check`: the corrected sentence came back with only its capital letter).
- **Streamed.** The request asks llama-server to stream (chunked server-sent events); `wr_stream_feed` reads them
  strictly (status 200, chunk sizes, lines of at most 64 KB, JSON events) and keeps at most 16 KB of text, which the
  panel shows as it comes (`lord.writing.peek`). 1,536 tokens at most, temperature 0.2 (0.4 to improve), top-k 20,
  top-p 0.8 (Qwen3's non-thinking settings), no thinking (`--reasoning off` and `enable_thinking: false`).
- **What the models do (measured, `build/writing-real-check`, 2026-11-26).** Qwen3 1.7B now corrects accents,
  capitals and punctuation reliably ("ayer fuimos al cine y la pelicula…" → "Ayer fuimos al cine y la película…,
  pero había…"; "i realy dont know wich one is beter" → "I really don't know which one is better") and translates
  well-formed sentences, but it misses words that are themselves real words ("esas" for "estás"), changes little when
  asked to improve, and loosely translates informal text. Qwen3 4B does all of these clearly better. The panel shows a
  tip toward 4B under Improve and Translate while the model is 1.7B (`writing.panel.tip4b`).
- **Plain text.** `sdk/writingmodel.js` `cleanOutput` drops control characters and bidi overrides, a `<think>` block,
  echoed markers and a code fence around the whole answer; nothing is interpreted. **Replace / Copy / Discard**:
  nothing changes without the click, and Replace only where the selection was if it still holds the text sent
  (`stillThere`).
- **Limits.** Never in a password field; at most 4,000 characters (the engine's context is 4,096 tokens); one job at a
  time in the engine's queue with Sentinela's; Discard or Esc cancels the stream (`lord.writing.cancel`). Nothing goes
  online: the same engine and sandbox as Sentinela ([SENTINELA.md](SENTINELA.md)).
- **The Brain's sheets:** "Used by" lists the functions that use the model from `FUNCTIONS` (`llmFunctions`: Writing,
  Sentinela) plus Monitor's "Investigate with Lumo"; "Try it" translates with that model whenever Writing or Sentinela is
  on (`llmTryReady`), not only with Sentinela.
- **Web apps:** lord-web's right-click menu has the same ✨ AI row for a page's selection; Replace goes back only to a
  focused text field whose selection is unchanged (`docs/WEBAPPS.md`, "✨ AI on a page's selection"). The menu marks the
  focused field (`webedit.h` `WEBAI_MARK_JS`); Replace waits until the web app's window has the keyboard back (the
  shell's panel holds it while open, and WebKit then treats the page as unfocused: the first version required
  `document.hasFocus()` and so never replaced anything, WhatsApp's message box on ws164 included), then inserts the
  text as a paste would, `document.execCommand('insertText')`, which runs the page's `beforeinput` and `input`
  listeners, so a rich editor such as WhatsApp's Lexical takes it into its own model (`WEBAI_REPLACE_JS`); WebKit's own
  InsertText only when the field didn't change. Only in that same field, still focused, editable, not a password, with
  exactly the selection sent; a refusal is logged with one word of a closed list (`webai_refusal`: `gone`,
  `other-field`, `password`, `read-only`, `not-editable`, `no-selection`, `selection-changed`, `no-menu`, `no-focus`,
  `bad-text`, `no-answer`). "Translate to" names every language in the interface's language, Chinese's two scripts
  included (`languages.json`, `tools/gen-language-names.mjs`; "zh-Hans" showed raw before).
- **Tests:** `build/llm-engine-test` (the requests: task, examples, sampling, the text last and once; streams whole
  and byte by byte, malformed answers, the 16 KB cap, a fuzz, a streamed job through the fake engine seen while it
  streams, a translation, cancel, bad arguments); `server/tests/writing_model_test.mjs` (also: every translation
  target named in every catalog); `server/tests/spell_scene` (screenshots of the panel streaming and done);
  `build/web-ai-replace-check` (Replace in a textarea, a contenteditable box and a Lexical-like editor on the real
  WebKit, its refusals); `build/writing-real-check [model] [threads]` (by hand: the test sentences through the real
  engine and model, as lord-server sends them); `build/lumo-test` (the command against a private bus).

### Writing from the terminal: `lumo` (in development, awaiting the maintainer's approval)

```
lumo corregir "texto"        lumo correct "text"          (fix)
lumo mejorar "texto"         lumo improve "text"
lumo resumir < archivo       lumo summarize < file        (shorten, acortar)
lumo traducir en "texto"     lumo translate pt "text"     (a code or a name: en, inglés, Portuguese…)
echo texto | lumo corregir   lumo estado | status         lumo --help | --ayuda
```

`/usr/bin/lumo` (a link to `/usr/lib/lordos/bin/lumo`, `server/src/cli/lumo.c`) never reaches the model itself. It asks
the person's running session over the session bus: **`org.lordos.Writing1`** at `/org/lordos/Writing1`, served by
lord-server (`server/src/ui/writebus.c`, on the portal's connection): `Status() → (state, model, engine)`,
`Start(action, target, text) → id`, `Read(id) → (state, text)` (queued, starting, writing with the text so far, done,
failed with an error code) and `Cancel(id)`. lord-server runs the panel's own job (`llm_submit_write`, `writing.c`'s
prompts, the local engine in its sandbox; nothing goes online). The bus says each caller's uid before its first call
is served, and another uid gets AccessDenied; a job belongs to the connection that started it, nobody else reads or
cancels it, and it is cancelled when that connection leaves; at most 4 at once. The switch and the model are the
panel's: the shell tells lord-server (`lord.writing.serve`); off, the command says to turn it on in Settings → AI →
Writing (exit 3). The answer is written to stdout as it streams, safe for a terminal (`server/src/llm/lumo.c`
`lumo_out_feed`: no control or bidi characters, no echoed markers or `<think>`). Verbs in English and Spanish; messages
by the locale (`assets/locales/<lang>/lumo.json`). Exit status: 0 done, 1 the model failed or took too long, 2 a wrong
command, 3 Writing off or no model or engine, 4 no LordOS session, 5 the text empty, too long (4,000 characters) or
not UTF-8, 130 interrupted (the job is cancelled). No Debian package installs a `lumo` command (packages.debian.org,
trixie and sid contents, 2026-11-26).

### The engine's threads

The Brain's processor threads are the person's (asked by the maintainer on 2026-11-26): **Settings → AI → Where they
run → "Procesador: núcleos para el Cerebro"**, a slider under the Brain's Automático / Procesador / Placa de video, from
**Automático (N)** (half the online cores, at least 1, at most 16: 10 on a 20-thread i9-10900, 4 on a Raspberry Pi 5)
to every online core, with the hint that more cores answer faster but load the computer more. It counts on the
processor and for the layers a GPU couldn't take (a partial offload); with every layer on the card it barely matters.

- **The setting**: `settings.brain.cpuThreads`, 0 automatic (the default; anything that isn't a whole number ≥ 0 reads
  as 0), else a count. `setBrainThreads` → `lord.sentinela.threads(n)` → `llm_set_threads` (`engine.c`), sent once the
  slider rests (700 ms; at start at once). A count above the cores is clamped to them (a setting made on a bigger
  computer); the state (`lord.sentinela.state()`) gives `cpus`, `threadsAuto`, `threads` (the next start's) and
  `runThreads` (the running engine's).
- **lord-llm** gets the count in one more environment word, `LORD_LLM_THREADS=<n>`, and checks it on its side
  (`llmjail_threads_parse`): digits only, no sign, space or leading zero, 1..its online cores (at most 64, the most
  lord-sessiond writes in `cpu.max`); anything else exits 2 before the sandbox, none at all is automatic. The engine's
  `-t` and `-tb` are that count (`llmjail_argv`: the backend's fixed list with only those two values written in;
  `--threads-http` stays 2) and `lordos-sentinela.scope`'s `cpu.max` is 100 % per thread (`launcher.c`). Nice 15,
  `SCHED_BATCH` and idle I/O stay, so the desktop still comes first.
- **A change** restarts an idle engine at once (same model and backend, by the engine's thread; its idle time keeps
  counting from its last answer) and a busy one at its next job; the same effective count restarts nothing.
- Measured with `build/writing-real-check` (16 jobs, Qwen3 1.7B, an i9-10900, no cgroup, load ≈ 2): 2 threads 106 s
  (6.1 s a job), 4 threads 61 s (3.4 s), 8 threads 41 s (2.0 s). Before 0.8.9.6.2 the engine always ran with
  `-t 2 -tb 2` in a scope of two cores.

## Capabilities

Each capability is a collection of models; each model is a card (size, memory, license, state: ✓ installed, ☁ not
installed, coming soon, the download's progress) and a sheet with its facts, what uses it, **try it** where it really
works, and its actions.

- **Brain**: Qwen3 1.7B and 4B, and the engine. Install (the engine first when missing), Remove, Use (the model
  Sentinela and Lumo use, `settings.sentinela.model`). Try it: *Translate with Lumo* (`translateWithLumo`), when
  Sentinela is on and the model is the one in use. The engine runs on the graphics card when it can (in development,
  "The Brain on the GPU" below), else on the processor.
- **Voice**: one card per language (its standard model and own voice: install standard or HD, remove, listen to the
  recorded sample, try). Its page holds everything the old Voice page had: Read aloud, ❖Esc, the player, Live Speech,
  where speech is made (Auto / CPU / GPU), quality, a language's two models and its voices as cards (▶ sample, Try,
  Use, Download, Remove), the recorded samples' package, and the person's own voices (record or from a file).
- **Vision**: Tesseract (included: Screenshots' "copy the text") and one card per interface language: Debian 13's
  `tesseract-ocr-<lang>` data, installed and removed with the package engine (`pkg-install` / `pkg-remove`, an
  administrator, only names of `OCR_LANGS`). Try it: *Read a picture…* (lord-media decodes it, lord-ocr reads it).
- **Ear** (Whisper Base, Small, Large v3 Turbo: ggml sizes 148, 488 and 574 MB, MIT) and **Image** (Real-ESRGAN x4 and
  x4 Anime, ~64 and ~17 MB, BSD-3-Clause; the x4 model made LordOS's 4K wallpapers, off LordOS): **coming soon**, their
  facts only, no install button. Image's sheet shows a before-and-after crop (`assets/ai/esrgan-compare.jpg`). Both
  need a new helper, and that needs the maintainer's security approval first.

## The bottom

The space the models take (measured on disk: `/usr/share/lordos/voice`, `~/.local/share/lordos/voice`,
`/usr/share/lordos/llm`, Tesseract's `tessdata`), where they run (the Voice's real Auto / CPU / GPU setting from
`voice.json`; the Brain's own Auto / CPU / GPU, below, with what the engine really used, and its processor cores ("The
engine's threads"); Vision on the processor) and the
privacy note: no model uses the network.

## The Brain on the GPU (in development, awaiting the maintainer's approval)

Asked by the maintainer on 2026-11-26. Until now the engine ran on the processor only (`-ngl 0 -dev none`). Now
"Where they run" has **Automático / Procesador / Placa de video** for the Brain, as for the Voice
(`settings.brain.backend`, default `auto`; `setBrainBackend` → `lord.sentinela.backend(mode)` → `llm_set_backend`), and
under it the line of what the engine used (`brainWhere`, `sdk/aistore.js`): *Cerebro: en la placa de video (Vulkan, AMD
Radeon RX 580 Series)*, *… 20 de 29 capas; el resto en el procesador* when only some fit, or on the processor with the
reason (chosen, no usable card, the card couldn't start it, the card ran out of memory, an untested card on arm64).

- **The engine.** LordOS's `llama.cpp` package was already built with `GGML_BACKEND_DL` and `GGML_CPU_ALL_VARIANTS` (every
  CPU variant a module, the best loaded at run time) and `GGML_VULKAN` (`libggml-vulkan.so`, used only when a Vulkan
  device exists; it needs `libvulkan1` and Mesa's `mesa-vulkan-drivers`, which the package recommends; built with
  `libvulkan-dev`, `glslc` and `spirv-headers`). Nothing changes in it. **CUDA** is a separate, optional package,
  `llama.cpp-cuda` (`apt/recipes/llama.cpp-cuda/build.sh`): only `libggml-cuda.so`, from the same pinned source, built
  in the `nvidia-cuda` image (the build image plus NVIDIA's CUDA 13.5 compiler from NVIDIA's own Debian 13 repository,
  its key pinned by fingerprint in `apt/keys/nvidia-cuda.asc`) for Turing and newer cards; it depends on NVIDIA's
  `cuda-cudart-13-5`, `libcublas-13-5` (about 600 MB) and `libcuda1 (>= 590)`, so it can only be installed where
  NVIDIA's repository is, and it is never a Store page (`section: "addon"`). Nothing proprietary enters the base system.
- **Choosing** (`server/src/llm/engine.c`, `pick_backend`): the processor when chosen; else CUDA when
  `libggml-cuda.so` is in `/usr/lib/llama.cpp` (root's, writable by no one else) and `/dev/nvidiactl` exists; else
  Vulkan when `libggml-vulkan.so` is there and a render node exists; else the processor. On arm64 "auto" keeps the
  processor (the Raspberry Pi's V3D isn't known to run the engine well); "Placa de video" tries it.
- **Arguments** (fixed per backend, `llmjail.c`): the processor `-ngl 0 -dev none`; Vulkan `-ngl auto -dev Vulkan0 -lv 4`;
  CUDA `-ngl auto -dev CUDA0 -lv 4`. `-ngl auto` is llama.cpp's `--fit`: as many layers as fit in the card's free
  memory with 1 GB to spare, the rest on the processor. The first device is the one llama.cpp lists first (here the
  RX 580 before the Intel UHD 630).
- **What it used.** The engine's stderr is a pipe lord-server reads on a thread for as long as it runs: the device's
  name from `using device Vulkan0 (…)` (printable ASCII, up to its first " (", at most 80 characters), `offloaded N/M
  layers to GPU`, and the card's out-of-memory lines; nothing else is interpreted. `lord.sentinela.state()` gives
  `mode`, `gpus`, `backend`, `device`, `layers`, `layersTotal` and `fallback`.
- **Falling back.** A GPU that can't start the engine, has no room for a single layer, or runs out of memory while
  answering (the engine dies or logs it) is given up on until the choice changes, and the job runs again at once on
  the next backend, the processor last (a streamed Writing answer too, before its first word).
- **Sandbox** (docs/SENTINELA.md "Sandbox"; AGENTS.md, awaiting approval): only for the backend in use, Vulkan reaches
  the render nodes (`/dev/dri/renderD*`, never `card*`) with their ioctls and the parts of `/sys` Mesa reads; CUDA
  reaches `/dev/nvidiactl`, `/dev/nvidia<N>`, `/dev/nvidia-uvm` and `/proc/driver/nvidia`. Nothing else changes.

### Measured (2026-11-26, the development PC)

Intel Core i9-10900 (10 cores), AMD Radeon RX 580 8 GB (RADV, Mesa 25.0.7), Debian 13, LordOS's llama.cpp 0.7.0
(b11669). `llama-bench -p 512 -n 128 -r 3`, tokens per second (prompt reading / generating):

| Model | Processor, 2 threads (the engine's setting until the cores slider) | Processor, 8 threads | RX 580, Vulkan (every layer) |
|---|---|---|---|
| Qwen3 1.7B Q4_K_M | 72.7 / 20.1 | 259.8 / 25.0 | **1038.4 / 110.4** |
| Qwen3 4B Q4_K_M | 29.4 / 8.2 | 95.3 / 9.8 | **422.6 / 51.8** |

A Sentinela analysis (the real summary and prompt, the engine in lord-llm's sandbox under the scope's memory cap,
`build/llm-engine-test --real`, 51–61 tokens): Qwen3 1.7B **0.85 s warm on Vulkan against 9.3 s on the processor**
(3.5 s against 11.7 s cold, the engine's start included, 2.5 s on Vulkan); Qwen3 4B 1.8 s against 23.3 s warm (5.3 s
against 26.7 s cold). Generating inside the sandbox: about 98 tokens/s on Vulkan. Both models fit whole in the RX 580
(29/29 and 37/37 layers).

Not tested yet: CUDA (no NVIDIA card here; `llama.cpp-cuda` is written, not built: it needs Docker and NVIDIA's
repository on the build machine), Intel and NVIDIA cards through Vulkan, and the Raspberry Pi's V3D.

## Pages and links

The capability pages and Sentinela's are Settings sub-pages (`apps/settings/subpage.js`: "ai/voice", "ai/sentinela"),
so the mouse's Back and Forward and Esc walk them. Other apps open one with
`wm.open('settings', { args: { section: 'ai', page: 'sentinela' | 'voice' | 'brain' | … } })`.

## Tests

`node server/tests/ai_store_test.mjs` (in `make test`); `server/tests/app_scene_headless.sh settings_ai_scene OUT
default en dark` (and `ar` for right to left) drives the page headless and keeps screenshots.

## Assets

`assets/ai/icons/<name>-symbolic.svg` are the capability icons; `<name>-96.png` are the white renders the page draws
(`rsvg-convert -w 96` of the SVG with `stroke="#fff"`: an SVG icon from the assets is drawn by lord-media only from
the installed `/usr/lib/lordos/assets`, and PNGs work in the headless scenes too). `assets/ai/esrgan-compare.jpg` is a
crop of one 4K wallpaper, ordinary scaling against Real-ESRGAN. `tools/iso/mkdeb.sh` installs `assets/ai`.
