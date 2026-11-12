# Translating LordOS

LordOS is translated by people who speak the language. This guide shows how to add a new language, or finish a
partial one, starting from the English files, and how to send it as a pull request. You don't need to write code:
every text is in plain JSON files. Accepted translations ship in the next release.

The languages LordOS ships, and how new ones are chosen, are on [lordos.org/language](https://lordos.org/language/).
If you'd rather ask for a language than translate it, use the
[language request form](https://github.com/lordbasex/LordOS/issues/new?template=language-request.yml).
The technical reference (plural rules, formats, right to left, fonts, input methods) is
[docs/LANGUAGES.md](LANGUAGES.md). The lordos.org website itself is in English, Spanish and Portuguese today; this
guide is about the LordOS interface.

## How the texts are organised

```
assets/locales/
  en/                 English: the reference and the fallback for every other language
    meta.json         the language's identity: code, name, plural rule, region, direction
    format.json       month and day names, date patterns, separators, 12/24-hour clock
    shell.json        one file per namespace: the desktop (shell), Files (files), Settings (settings)…
    files.json
    …
  es/  pt/  fr/  it/  lt/  …
```

- A file is a namespace and holds `"key": "text"` pairs. `t('files.items')` in the code is the key `items` in
  `files.json`.
- Every language has the same files and keys as `en/`. A key that is missing in a language falls back to the user's
  next preferred language and then to English, so a half-done translation never breaks anything.
- English is the source. Always translate from `en/`, even if you also read another language for reference.

## 1. Create the folder

Pick the language's code: the ISO 639-1 code (`de`, `ja`, `uk`), or ISO 639-3 if it has none, plus a script or region
only when it matters (`zh-Hans`, `zh-Hant`, `pt-PT`). Then create `assets/locales/<code>/` with these two files and
copy the English namespaces you are going to translate into it.

`meta.json`:

```json
{
  "id": "de",
  "name": "Deutsch",
  "english": "German",
  "plural": "one-other",
  "region": "de_DE",
  "direction": "ltr",
  "partial": true
}
```

- `name` is the language's name **in the language itself** (Deutsch, 日本語, العربية); `english` is its English name.
- `plural` is one of the rules below. `region` is the glibc locale most of its speakers use (`de_DE`, `pt_BR`).
- `direction` is `"rtl"` for languages written right to left (Arabic, Hebrew, Persian, Urdu…): the whole interface
  is mirrored for them.
- `"partial": true` says the language translates only part of LordOS (see step 2). Remove it when every key is
  translated.

`format.json` starts as a copy of `en/format.json`. Translate the month and day names (`months`, `monthsShort`,
`weekdays`, `weekdaysShort`, starting on Sunday), reorder the date patterns (`short`, `long`, `dayMonth`,
`numeric`) with the placeholders `{weekday}`, `{wd}`, `{month}`, `{mon}`, `{day}`, `{dd}`, `{mm}`, `{yyyy}`, and set
`decimal`, `group` and `hour12` (with `am`/`pm` if the language uses a 12-hour clock). `regions` can override any of
these for one country (`pt_PT`, `en_GB`). Use the forms your language's grammar needs: Lithuanian, for example,
writes «2026 m. lapkričio 8 d., sekmadienis».

`node tools/i18n-pack.mjs new de "Deutsch" German one-other de_DE` writes the same `meta` with every English text as
one file, `lordos-lang-de.json`, which some translators find handy as a reference; the pull request itself carries the
folder, one file per namespace.

## 2. Translate the core first

A new language starts **partial**: it translates the core of LordOS, the parts everyone sees in the first minutes,
and the rest shows in English until it is translated. The core set is listed in `tools/i18n-core.json` (1,052 of
about 12,400 English keys):

| File in `assets/locales/en/` | What it is | Scope |
|---|---|---|
| `greeter.json` | the login screen | whole file |
| `shell.json` | the desktop: menu bar, Dock, launcher, Control Center, notifications | whole file |
| `files.json` | Files | whole file |
| `installer.json` | the installer | whole file |
| `about.json`, `appabout.json` | About LordOS, About an app | whole files |
| `apps.json` | the app names | whole file |
| `powerbox.json` | the open and save dialogs | whole file |
| `forcequit.json`, `calculator.json` | Force Quit, Calculator's name | whole files |
| `settings.json` | Settings | only `section.*`, `lang.*`, `region.*`, `pack.*`, `system.*`, `tz.*`, `admin.*` and `change`, `light`, `dark`, `mode`, `noResults`, `search` |
| `prefs.json` | preferences | only `time.*` (Date & Time formats) |

Translate `apps.json` first: the app names appear everywhere, so deciding them early keeps the rest consistent. After
the core, any other namespace can follow in any order (`settings.json` in full, `mail.json`, `terminal.json`…), one
pull request at a time if you like.

## 3. Placeholders, plurals and style

**Placeholders** in braces are filled in by LordOS and must stay exactly as they are, though you may move them
inside the sentence:

```json
"net.keyringLabel": "Shared folder {name} on {addr}"      (files.json, English)
"net.keyringLabel": "Freigegebener Ordner {name} auf {addr}"
```

Don't translate, rename, add or drop a `{placeholder}`; `i18n-check` reports any difference with English.

**Plurals.** A text that depends on a number is an object with one entry per plural form:

```json
"items": { "one": "{count} item", "other": "{count} items" }
```

Give exactly the forms of your language's rule (`meta.json` → `plural`); `other` is always allowed:

| Rule | Forms | Languages (examples) |
|---|---|---|
| `one-other` | `one` (1), `other` | English, Spanish, Italian, German, Dutch, Swedish, Greek, Turkish, Hungarian, Finnish, Bulgarian |
| `zero-one-other` | `one` (0 and 1), `other` | Portuguese, French, Hindi, Bengali, Persian |
| `none` | `other` only | Japanese, Chinese, Korean, Thai, Vietnamese, Indonesian, Malay |
| `east-slavic` | `one` (1, 21), `few` (2–4, 22), `many` (0, 5–20) | Russian, Ukrainian |
| `polish` | `one` (1), `few` (2–4, 22), `many` (0, 5–21) | Polish |
| `czech-slovak` | `one` (1), `few` (2–4), `other` | Czech, Slovak |
| `lithuanian` | `one` (1, 21), `few` (2–9, 22), `other` (0, 10–20) | Lithuanian |
| `romanian` | `one` (1), `few` (0, 2–19), `other` (20, 100) | Romanian |
| `arabic` | `zero`, `one`, `two`, `few` (3–10), `many` (11–99), `other` (100) | Arabic |

If your language needs a rule that isn't there, say so in the pull request: rules are code (`sdk/plurals.js`, with
tests) and we add them.

**Style.**

- Translate the meaning, not word by word, and use the words people see on their own phones and computers in your
  language. Keep texts about as short as the English ones: buttons and menus have little room.
- Be consistent: once you choose a word for Trash, Dock, Files, Settings, Store or Cloud, use it everywhere. The terms
  in `apps.json` and `shell.json` are the glossary for the rest of your translation.
- **Never translated:** LordOS, Lumo, Lord Flow, Lord Classic and LordBench.
- No third-party brand or product names (other systems, their apps or features): LordOS uses its own generic names
  (Search, Apps, Night Light, Quick Preview…). `tools/brand-check.mjs`, run by `i18n-check`, fails on them.
- Use the typographic punctuation of your language (« », „ “, 「」). Dates and numbers come from `format.json`, not
  from the texts.
- Right-to-left languages: write the text naturally; the interface mirrors itself, and English names or numbers
  inside an Arabic, Persian or Urdu sentence are laid out by LordOS's bidi engine.

## 4. Check it

From the repository's root, with Node.js:

```sh
node tools/i18n-check.mjs        # every language, plus the brand and error-code checks
node tools/i18n-check.mjs de     # only your language
```

For a partial language it prints its coverage (of the whole catalog and of the core set) and the namespaces that are
complete or started, instead of failing on missing keys. It fails on keys that English doesn't have, on placeholders
that differ from English and on plural objects with the wrong forms. It must pass before the pull request is merged.

To see your translation on LordOS itself, make a pack and add it in Settings:

```sh
node tools/i18n-pack.mjs export de      # writes lordos-lang-de.json from assets/locales/de
```

Copy `lordos-lang-de.json` to the Downloads folder on LordOS and use **Settings → Language & Region → Add**. It is
installed for your user only (`~/.local/share/lordos/locales/`), without root, and you can then pick it in the list of
preferred languages. Screenshots
of what you checked are welcome in the pull request, especially for scripts and right-to-left languages.

## 5. Open the pull request

1. Fork [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS) and create a branch, for example
   `i18n/de`.
2. Commit only the files under `assets/locales/<code>/`, signed off as every contribution
   ([Developer Certificate of Origin](https://developercertificate.org/)):

   ```sh
   git add assets/locales/de
   git commit -s -m "i18n: German core set"
   ```

3. Open the pull request against `main`. Say which namespaces you translated, the plural rule and region you chose,
   and anything you weren't sure about. Someone else who speaks the language may review it.

You don't have to touch code: a language in `assets/locales/` shows up in Settings and on the login screen by itself,
and the maintainers add it to the website's list. Once merged, the translation ships in the next release (pre-releases first), and every later release can
complete it further. Contributions are accepted under the MIT license, following
[CONTRIBUTING.md](../CONTRIBUTING.md) and the [Code of Conduct](../CODE_OF_CONDUCT.md).
