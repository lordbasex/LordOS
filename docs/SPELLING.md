# Spelling (in development)

LordOS checks spelling in its own text fields as you type: a misspelled word gets a wavy red underline, and a right
click on it starts the field's menu with suggestions, **Add to dictionary** and **Ignore**. Everything happens on the
computer; nothing goes online. Approved by the maintainer on 2026-11-26. The latest release is 0.8.9.6.1.

With text selected, the same menu offers **✨ AI ▸** (correct, improve, shorten, translate) when Writing is on: see
[AI.md](AI.md), "Writing".

## Pieces

| Piece | Where | What it does |
|---|---|---|
| `lord-spell` | `server/src/spell/lordspell.c` | The sandboxed helper; the only program that loads Hunspell |
| The frames | `server/src/spell/spellproto.{c,h}` | The channel's strict, length-capped requests and answers, parsed on both sides |
| The service | `server/src/spell/spellsvc.{c,h}` | Starts lord-spell with the chosen dictionaries, talks to it on its own thread, the person's dictionary |
| The languages | `server/src/spellang.{c,h}` | The rule shared with lord-web (copied unchanged from `feat/web-context-menu`) |
| `lord.spell` | `server/src/ui/js_spell.c` | The desktop's JS API (not in isolated apps) |
| The model | `sdk/spellmodel.js` | Which words, which ranges, where the lines go, the menu's rows, the packages (pure, tested) |
| The runtime | `sdk/spell.js` | Settings, the cache, the debounced batches, the suggestions |
| The fields | `sdk/components.jsx`, `sdk/editorextras.jsx` | `TextInput`, `TextArea`, Notes' and Mail's editors |
| The line | `server/src/ui/paint.c` `paint_wavy` | The engine's `wavyColor` style |
| Settings | `apps/settings/Spelling.jsx` | Settings → Keyboard: on/off, "Spelling: languages", more dictionaries, your dictionary |

## lord-spell

lord-server never links or loads Hunspell. `spellsvc.c` starts `lord-spell` from the fixed path next to itself (root's
or the user's, not writable by group or others) with no arguments, an empty environment and:

- fd 0: a socketpair, the channel; fds 1 and 2: `/dev/null`;
- fd 3 + 2i and 4 + 2i: language i's `.aff` and `.dic`, opened read-only by lord-server from `/usr/share/hunspell`
  (the real file may be in `/usr/share/myspell`; Debian's `es_AR.aff` is a link to `es_ES.aff`): regular files of at
  most 64 MB, owned by root or the user and not writable by others, names `[a-z]{2,3}(_[A-Z]{2,3})?`.

The helper reads the first frame (`'L'`: the languages' names), dlopens `libhunspell-1.7.so.0` (`RTLD_NOW |
RTLD_LOCAL`) only from Debian's fixed library folders after checking that the folder, its parents and the real file
are root's and not writable by group or others, creates one Hunspell per language through `/proc/self/fd`, closes every
descriptor but the channel and applies `sandbox_apply_helper` (scope `lordos-spell`: no files, no network, no new
sockets, no exec, a syscall allowlist, 512 MB). Only then does it answer (`'R'`: which languages loaded) and read words.
Dictionaries in UTF-8 and ISO-8859-1 are used (the second converted both ways); any other encoding leaves that language
off.

### The frames

Four bytes (`'S'`, the operation, the payload's length, little endian, at most 8,192) and the payload:

| Op | Direction | Payload |
|---|---|---|
| `L` | lord-server → helper, once | n (1–4), n × (length, tag) |
| `R` | helper → lord-server | n, n bytes (1 loaded, 0 not) |
| `C` | check | mask (languages, bit i = tag i), n (1–64), n × (length, word) |
| `c` | answer | n, n bytes (1 a language of the mask knows it) |
| `G` | suggest | mask, (length, word) |
| `g` | answer | n (0–8), n × (length, suggestion) |

A word is 1–64 bytes of well-formed UTF-8 (no overlong forms, surrogates, NUL), no C0 or C1 control, no space and no
ASCII punctuation but `'`, `-` and `.`; a suggestion may also have single spaces ("a lot"). Both sides use the same
parsers; anything else — a short read, a frame out of order, a length that disagrees with its payload, an answer for
another count — ends the channel, and lord-server kills a helper that misbehaves or doesn't answer within 4 s (15 s to
load). Words lord-server can't send (too long, not a word) count as good and are never sent.

The helper stops 10 minutes after its last answer and restarts when the languages change; after three failures in a
row it rests a minute.

## Which words

`sdk/spellmodel.js` `spellWords`: runs of letters and marks, with `'`, `’` or `-` inside. Left alone: anything in a
chunk (between spaces) that is a link, an e-mail address, a path or code (`://`, `@`, `www.`, `/` or `~/` first, `_`,
`{}<>=#$%^*|\`, a domain); words with digits next to them; abbreviations in capitals; camelCase; one letter; words
longer than 64 bytes. **Scripts:** a word is checked only when a dictionary in use is written in its script (Latin,
Cyrillic, Greek, Arabic, Hebrew, Devanagari, Bengali, Thai, Hangul…); without an Arabic dictionary nothing Arabic is
underlined, so right-to-left text stays clean. The word being typed (ending at the caret) isn't underlined until the
caret leaves it.

New words go to lord-spell 350 ms after the last key, in batches of 64; verdicts are cached per language set (20,000
words). The wavy line is one node of 3 px under the word (`wavyColor`, painted by the engine as one alpha mask of a
sine wave), so a line coming or going damages only its own small box. In right-to-left text and mixed text the line
follows the boxes the selection would have (`lord.text.carets`).

## Fields

- **`TextInput` and `TextArea`** (every app): on by default. `spellCheck={false}` turns it off for a field; a password
  field (`secure`) is never checked and never offers ✨ AI. `SearchField` and LordOS's address, host, user, path,
  port, key and URL fields pass `spellCheck={false}`.
- **Notes and Mail's body** (their own editors): `sdk/editorextras.jsx` (`useEditorSpell`, `EditorSquiggles`,
  `editorMenuRows`).
- **Not checked:** the Terminal, Text Editor (a code editor; plain text mode is a follow-up), isolated `.lpk` apps (no
  `lord.spell` in their broker), and lord-web's pages (WebKit checks those itself with the same languages and words).

## Languages

Settings → Keyboard → **Spelling: languages** lists the installed dictionaries; at most 4 can be ticked, and a word is
correct when any of them knows it. With none ticked, the defaults are `spellang.h`'s rule, the one lord-web uses: the
interface language's dictionary (the region first, `es_AR` for Spanish in Argentina, then the language's usual one,
`es_ES`, `pt_BR`, `en_US`…), then English, only installed ones.

The images ship `hunspell-en-us`, `hunspell-es` and `hunspell-pt-br` (and `libhunspell-1.7-0`): `tools/iso/build.sh`,
`tools/rpi/build-img.sh` and the `lordos` package's Recommends (`tools/iso/mkdeb.sh`). **More languages** installs
Debian 13's other dictionaries through the package engine (`pkg-install`, an administrator), only the names of
`SPELL_PACKAGES` (English UK, Portuguese, French, German, Italian, Dutch, Polish, Russian, Ukrainian, Turkish, Arabic,
Bulgarian, Bengali, Catalan, Greek, Hindi, Hungarian, Indonesian, Lithuanian, Romanian, Swedish, Swahili, Thai,
Vietnamese, Korean). Debian 13 has none for Azerbaijani, Persian, Finnish, Filipino, Japanese, Malay, Urdu or Chinese.

## The person's dictionary

One store for LordOS and the web apps: **enchant's word lists**, `$XDG_CONFIG_HOME/enchant/<tag>.dic` (else
`~/.config/enchant/<tag>.dic`), one word per line — the files lord-web's WebKit (through enchant) reads and its "Add to
Dictionary" appends to. LordOS reads the lists of the languages in use; **Add to dictionary** writes the word into each
of them (as WebKit's Learn Spelling adds to each of its dictionaries), and Settings → Keyboard → Your dictionary removes
it. lord-server writes them (a `.new` file created with `O_EXCL | O_NOFOLLOW`, 0600, then renamed; lines it doesn't
understand are kept as they are); the helper never sees them. A word kept in lower case is good capitalised; one kept
capitalised (a name) stays so. WebKit reads a list when it loads a language, so a word added in LordOS reaches an open
web app after it is reopened. **Ignore** lasts for the session, in every LordOS field.

## Tests

- `build/spell-test` (`make test`, ASan and UBSan): words and tags; every builder's frame parsed back; truncated,
  extended, mutated and random payloads (400,000) never crash and what parses is valid; the person's dictionary file;
  the service with the real helper and Hunspell on small dictionaries the test writes (UTF-8 and Latin-1): checks in
  three languages, suggestions, words never sent, spellang's defaults, the lists shared with enchant (a word WebKit
  learned read, 0600 files); `lord-spellprobe-test` answers only when opening `/etc/passwd`, `/usr/share/hunspell` and
  `/tmp`, an `AF_INET` and an `AF_UNIX` socket and `execve` were all refused after the sandbox; `lord-spellevil-test`'s
  malformed answer fails the job and the helper is killed; the system's `es_ES` when installed. Without libhunspell
  the end-to-end part prints "skipped: needs libhunspell".
- `server/tests/spell_model_test.mjs`: words, links, code, digits, scripts (right to left), ranges, the person's words,
  the word being typed, batches, a right click, UTF-16 offsets, the lines per row (wrapped and right-to-left), the
  menu's rows, the languages and the packages.
- `build/engine-test` (`test_damage_fuzz`, `test_damage_pipeline`) with `wavyColor` in the engine.
- Screenshots: `server/tests/spell_scene.mjs` (see below).

## Screenshots

`server/tests/app_scene_headless.sh spell_scene OUT default en light` (and `dark`) draws a `TextArea` with misspelled
words and the ✨ AI panel with a fake `lord.spell` and a fake `lord.writing`.
