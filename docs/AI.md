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
| Hands | doing things in LordOS with your permission (the agent) | **coming soon** |

Lumo waves when every piece that exists today is installed and sleeps while one you could download is missing.
**Talk to Lumo** stays disabled, with a "coming soon" line, until the Ear and the agent exist.

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
(`{ section: 'ai', page: 'writing' }`): off by default (`settings.writing.on`), like Sentinela; the menu shows it only
with Writing on, the engine installed and a model (the one chosen for Lumo, else the first installed).

- **It runs in the shell, never in the app.** `lord.writing` (`server/src/ui/js_sentinela.c`) exists only in the
  desktop's own JS context; an isolated app's broker has none. The SDK's field hands the selection to the shell's
  panel (`sdk/writing.jsx`); the app gets text back only through its field's change after the person clicks Replace.
- **Fixed prompts.** `server/src/llm/writing.c` holds the four system texts; JS sends only the action name, a target
  of the closed list of LordOS's 35 interface languages and the text. The text goes between `<<<TEXT nonce>>>` and
  `<<<END-TEXT nonce>>>` with a random nonce, stated as material and never instructions.
- **Streamed.** The request asks llama-server to stream (chunked server-sent events); `wr_stream_feed` reads them
  strictly (status 200, chunk sizes, lines of at most 64 KB, JSON events) and keeps at most 16 KB of text, which the
  panel shows as it comes (`lord.writing.peek`). 1,536 tokens at most, temperature 0.2 (0.4 to improve), no thinking.
- **Plain text.** `sdk/writingmodel.js` `cleanOutput` drops control characters and bidi overrides, a `<think>` block,
  echoed markers and a code fence around the whole answer; nothing is interpreted. **Replace / Copy / Discard**:
  nothing changes without the click, and Replace only where the selection was if it still holds the text sent
  (`stillThere`).
- **Limits.** Never in a password field; at most 4,000 characters (the engine's context is 4,096 tokens); one job at a
  time in the engine's queue with Sentinela's; Discard or Esc cancels the stream (`lord.writing.cancel`). Nothing goes
  online: the same engine and sandbox as Sentinela ([SENTINELA.md](SENTINELA.md)).
- **Web apps:** lord-web's right-click menu has the same ✨ AI row for a page's selection; Replace goes back only to a
  focused text field whose selection is unchanged (`docs/WEBAPPS.md`, "✨ AI on a page's selection").
- **Tests:** `build/llm-engine-test` (the requests, streams whole and byte by byte, malformed answers, the 16 KB cap,
  a fuzz, a streamed job through the fake engine seen while it streams, a translation, cancel, bad arguments);
  `server/tests/writing_model_test.mjs`; `server/tests/spell_scene` (screenshots of the panel streaming and done).

## Capabilities

Each capability is a collection of models; each model is a card (size, memory, license, state: ✓ installed, ☁ not
installed, coming soon, the download's progress) and a sheet with its facts, what uses it, **try it** where it really
works, and its actions.

- **Brain**: Qwen3 1.7B and 4B, and the engine. Install (the engine first when missing), Remove, Use (the model
  Sentinela and Lumo use, `settings.sentinela.model`). Try it: *Translate with Lumo* (`translateWithLumo`), when
  Sentinela is on and the model is the one in use. The engine runs on the processor only (`-ngl 0`).
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
`voice.json`; the Brain and Vision on the processor) and the privacy note: no model uses the network.

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
