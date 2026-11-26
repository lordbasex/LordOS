# Voice: text to speech (since 0.8.8.6)

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
| 4 | More languages: French, German, Italian and Dutch ([#215](https://github.com/lordbasex/LordOS/issues/215), approved on 2026-11-22) | in development (after 0.8.9.5-beta) |
| 5 | LordOS's own voice | to do |

## `say`

```sh
say "Hola, ¿cómo estás?"          # the language's chosen voice (else its own: Lola, Rafael, Alba, Estelle, Juergen, Giovanni, Daan)
say -l fr "Bonjour"                # es, en, pt, fr, de, it or nl; without -l, the text's own
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
  Dutch's standard model is Kyutai's own 6-layer folder, `<root>/models/nl/standard/` (with its `embeddings/`):
  audio.cpp publishes no Dutch GGUF, and lord-tts reads that folder as it reads an HD one (language `dutch`).
- **Voices:** `<root>/models/<lang>/[hd/|standard/]embeddings/<voice>.safetensors` (Kyutai's prepared voices), or
  `<root>/voices/<name>.wav` (16-bit or float WAV, 0.5–30 s; the voice is cloned from it). A voice asked for that isn't
  installed for the model falls back to the language's own, then to any installed one.
- **Speed:** a sentence at a time; each one plays (PipeWire's `pw-cat`) while the next is synthesized. Measured on an
  i9-10900 with an RX 580: about 10 s of speech in 1.3 s on the GPU and 3 s on the CPU, 220 MB of memory.
- `/usr/bin/say` is an alternative (`update-alternatives`), so another package's `say` can be installed too.
- **Status note** (since 0.8.8.6): once the first sentence is made, `say` writes `~/.cache/lordos/say/<pid>` with
  `lord-say 1 <vulkan|cpu> <hd|standard> <language> <speak|save>` and removes it when it ends (also on SIGINT and
  SIGTERM; notes of `say` processes that are gone are removed by the next one). Monitor → GPU reads it to say what the
  voice runs on (`docs/MONITOR.md`, "GPU"): lord-tts itself is sandboxed and not dumpable, so its `/proc` isn't
  readable. It is not lord-tts's folder: the helper can't write it.

## Text normalization (Spanish and Portuguese since 0.8.8.10; French, German, Italian and Dutch in development)

Pocket TTS's Spanish and Portuguese models, HD and standard, never learned to read digits or symbols: "Tengo 88 años y
nací en 1987" came out as "Tengo bronce proteaños y nací en Crombiquit…" with every HD voice, while the same sentence
written in words was read perfectly. audio.cpp's text preparation (`prepare_pocket_tts_text`) normalizes no numbers in
any language; the English models read digits because they learned them. So `say` writes them out as words itself,
for every language but English (`es` and `pt` first; `fr`, `de`, `it` and `nl` below; English text is left exactly as it is): `say_normalize()` in `server/src/tts/saytext.c`, a pure
function with its own tables and no dependency, run on the whole text once the language is known and before it is
split into sentences (so pieces are measured on what is spoken). Portuguese uses the Brazilian forms where the two
differ (dezesseis, dezenove, bilhão, "1.º de maio" → primeiro de maio). The rules follow the usual ones (num2words,
NVIDIA NeMo's text normalization), not their code.

| Written | Spanish | Portuguese |
|---|---|---|
| `88`, `1987`, `2026` | ochenta y ocho, mil novecientos ochenta y siete, dos mil veintiséis | oitenta e oito, mil novecentos e oitenta e sete, dois mil e vinte e seis |
| `100`, `101`, `1.000.000`, `1.000.000.000` | cien, ciento uno, un millón, mil millones | cem, cento e um, um milhão, um bilhão |
| `21 años`, `1 libro` (a count before a noun, under 1000) | veintiún años, un libro | vinte e um anos, um livro |
| `-5`, `−5` | menos cinco | menos cinco |
| `1.987`, `1 000`, `1.234,56`, `1,000,000` | thousands and comma decimals | the same |
| `3,5`, `3.5`, `3,05` | tres coma cinco, tres punto cinco, tres coma cero cinco | três vírgula cinco, três ponto cinco, três vírgula zero cinco |
| `0.8.8.10`, `192.168.0.1` | cero punto ocho punto ocho punto diez | zero ponto oito… |
| `25%`, `1,5 %` | veinticinco por ciento | vinte e cinco por cento |
| `1.º`, `1.ª`, `3.er`, `21.º` (1–100; above, the cardinal) | primero, primera, tercer, vigésimo primero | primeiro, primeira, —, vigésimo primeiro |
| `14:30`, `14:00`, `14h30`, `1:02:03`, `48 hs` | catorce y treinta, catorce horas, …, una hora, dos minutos y tres segundos, cuarenta y ocho horas | catorze e trinta, catorze horas, … |
| `15/11/2026`, `2026-11-15`, `15.11.2026` (day first) | quince de noviembre de dos mil veintiséis | quinze de novembro de dois mil e vinte e seis |
| `25 €`, `€25`, `25,50 €`, `1.000.000 €` | veinticinco euros, … con cincuenta, un millón de euros | vinte e cinco euros, … e cinquenta, um milhão de euros |
| `$10` (the user's region: below) | diez pesos in AR, MX, CL, CO, UY, DO, CU; diez dólares elsewhere or with no region | dez dólares (dez pesos in those countries) |
| `US$ 10`, `U$S 10`, `USD 10`, `10 USD`, `R$ 10`, `£2` | diez dólares (always), …, diez reales, dos libras (feminine: veintiuna libras) | dez dólares (always), …, dez reais, duas libras |
| `21 °C`, `1°C`, `90°` | veintiún grados Celsius, un grado Celsius, noventa grados | vinte e um graus Celsius, … |
| `#LordOS`, `#5`, `# Título` | LordOS, número cinco, Título | LordOS, número cinco |
| `nº 5`, `n.º 5`, `@user`, `A & B`, `2+2 = 4` | número cinco, arroba user, A y B, dos más dos igual a cuatro | …, A e B, dois mais dois igual a quatro |
| `10-20` (a rising range of up to 4 digits) | diez a veinte | dez a vinte |
| `24/7` | veinticuatro barra siete | vinte e quatro barra sete |

Digit by digit: a number that starts with 0 (`0800`, `007`), one of more than 9 digits written without separators
(phone numbers such as `1155551234`), and one of more than 12 even with them. `612 345 678` (three groups of three
with plain spaces) is a phone number read in its groups, not 612 million; plain spaces group thousands only after one
or two digits or once (`1 000`, `12 345 678`, `100 000`), and no-break or thin spaces always do. `11-5555-1234` is read
group by group. Ambiguities, as decided: one `.` followed by exactly three digits is a thousands separator (`1.987` is
1987, as written in Spanish and Portuguese), any other single `.` or `,` is a decimal point, read as written ("punto",
"coma").

**`$` by region** (maintainer's decision, 2026-11-16). A `$` alone is read as the currency of the user's region:
"pesos" where the country's own peso is written `$` (Argentina, Mexico, Chile, Colombia, Uruguay, the Dominican
Republic, Cuba), "dólares" (in both languages) anywhere else (Spain, the United States, Ecuador, El Salvador, Puerto Rico,
Brazil, Portugal…) and when no region is known. `US$`, `U$S` and `USD` are always dollars and `R$` always reais. The
region is the one in Settings → Language & Region (`"region"` in `~/.config/lordos/settings.json`, such as `es_AR`),
else `LC_MONETARY` or `LANG` in `~/.config/locale.conf`, else `say`'s own `LC_ALL`, `LC_MONETARY` or `LANG` (a
terminal's), else the computer's `/etc/default/locale`; only a value with a country counts (`es.UTF-8`, which
lord-server gives lord-say, doesn't). `-l` and `-v` don't change it. The region is a parameter of
`say_normalize()`, which stays a pure function; `say.c` finds it.

Digits inside words are split off and read with the same rules, up to 4 digits as a number and longer ones digit by
digit: `MP3` MP tres, `4K` cuatro K, `H264` H doscientos sesenta y cuatro, `COVID-19` COVID diecinueve, `1080p` mil
ochenta p. `C#`, `F#` and `C++` stay. Addresses and e-mails (`https://…`, `www.…`, `name@host.tld`) keep their letters,
lose the scheme, name their punctuation (punto/ponto, arroba, barra, guion/hífen, guion bajo/sublinhado, dos
puntos/dois pontos) and read their digits one by one: `info@lordos.org` info arroba lordos punto org.

Not covered: units (`5 km`, `10 GB` keep their abbreviation), fractions (`1/2` is "uno barra dos"), Roman numerals,
numbers above 999 999 999 999, the feminine of a count before a feminine noun (`1 casa` is "un casa"; money in libras
and hours are feminine), and the European Portuguese forms.

Tests: `server/tests/saytext_test.c` (267 checks in 0.8.8.10, 242 of them normalization: 126 Spanish, 82 Portuguese,
4 with English and other languages left untouched, and 30 for `$` by region; 541 with French, German, Italian and
Dutch, below).

Round trip, measured on 2026-11-15 on an i9-10900 with an RX 580: every installed Spanish and Portuguese voice (23 HD
and 22 standard each) said two sentences with the `say` of 0.8.8.9 and with this one, and whisper.cpp's `small` model
transcribed each recording (whole, and again from a third and two thirds in, since it sometimes drops a sentence); an
item counts when the transcript has the number, in digits or in words. Sentence 1 is "Tengo 88 años y nací en 1987.
Compré 3 libros por 25 euros. #LordOS es genial." (4 items); sentence 2 is "La inflación fue del 3,5% y el libro
cuesta 25,50 €. La reunión es a las 14:30 y quedé en 2.º lugar. El 15/11/2026 hacía 21 °C." (8 items: 3,5, the
percent, 25,50, 14:30, 2.º, 15 November, 2026, 21), each with its Portuguese translation (R$ for the price).

| Model | Sentence 1, before → after | Sentence 2, before → after |
|---|---|---|
| Spanish HD | 0/92 → 92/92 | 0/184 → 184/184 |
| Spanish standard | 51/88 → 84/88 | 73/176 → 170/176 |
| Portuguese HD | 1/92 → 87/92 | 10/184 → 172/184 |
| Portuguese standard | 70/88 → 85/88 | 49/176 → 176/176 |

Most of what is left is whisper's own spelling of what was said right ("80 e 8" for oitenta e oito, "20 e um").

### French, German, Italian and Dutch (in development, after 0.8.9.5-beta)

Kyutai's French, German, Italian and Dutch models were trained like the Spanish and Portuguese ones, on text with the
numbers written out, so `say` writes them out for these four too, with the same function, the same rules for what a
number is (separators, phone numbers, digits inside words, addresses, ranges, `$` by region) and tables of each
language's own. French is France's (soixante-dix, quatre-vingts, quatre-vingt-dix; not Belgium's or Switzerland's
septante, huitante, nonante), with the traditional hyphens. German and Dutch say years from 1100 to 1999 in hundreds
(`1987` neunzehnhundertsiebenundachtzig, negentienhonderdzevenentachtig), as they are spoken; with a separator (`1.987`)
it is a count.

| Written | French | German | Italian | Dutch |
|---|---|---|---|---|
| `21`, `22`, `71`, `80`, `91` | vingt et un, vingt-deux, soixante et onze, quatre-vingts, quatre-vingt-onze | einundzwanzig, zweiundzwanzig, … | ventuno, ventidue, settantuno, ottanta, novantuno | eenentwintig, tweeëntwintig, … |
| `100`, `101`, `200`, `1000`, `2026` | cent, cent un, deux cents, mille, deux mille vingt-six | hundert, hunderteins, zweihundert, tausend, zweitausendsechsundzwanzig | cento, centouno, duecento, mille, duemilaventisei | honderd, honderdeen, tweehonderd, duizend, tweeduizend zesentwintig |
| `23`, `103`, `1.000.000`, `2.000.000` | vingt-trois, cent trois, un million, deux millions | dreiundzwanzig, …, eine Million, zwei Millionen | ventitré, centotré, un milione, due milioni | drieëntwintig, …, één miljoen, twee miljoen |
| `1 Buch`, `1 libro`, `21 ans` (a count before a noun) | vingt et un ans | ein Buch (a capitalized noun) | un libro | één boek |
| `3,5`, `25 %` | trois virgule cinq, vingt-cinq pour cent | drei Komma fünf, … Prozent | tre virgola cinque, … per cento | drie komma vijf, … procent |
| ordinals | `1er` `1re` `2e` `2nd` `3ème` `21e`: premier, première, deuxième, second, troisième, vingt et unième | `3.` after an article or before a month: `am 3. Mai` am dritten Mai, `der 1. Platz` der erste Platz, `3. Oktober` dritter Oktober | `1º` `1ª` `11º` `23º`: primo, prima, undicesimo, ventitreesimo | `1e` `2de` `8ste` `21ste`: eerste, tweede, achtste, eenentwintigste |
| `14:30`, `14:00`, `1:02:03` | quatorze heures trente (also `14h30`, `14 h 30`), quatorze heures, une heure, deux minutes et trois secondes | vierzehn Uhr dreißig (also `14.30 Uhr`), vierzehn Uhr, eine Stunde, zwei Minuten und drei Sekunden | quattordici e trenta, quattordici, … | veertien uur dertig, veertien uur, … |
| `15/11/2026`, `15.11.2026`, `2026-11-15` | quinze novembre deux mille vingt-six (`01/05` premier mai; `1 janvier` premier janvier) | fünfzehnter November … (`am 15.11.` am fünfzehnten) | quindici novembre duemilaventisei (`1 maggio` primo maggio) | vijftien november tweeduizend zesentwintig |
| `25,50 €`, `1.000.000 €`, `£1`, `$10` | vingt-cinq euros cinquante, un million d'euros, une livre, dix dollars | fünfundzwanzig Euro fünfzig, eine Million Euro, ein Pfund, zehn Dollar | venticinque euro e cinquanta, un milione di euro, una sterlina, dieci dollari | vijfentwintig euro vijftig, één miljoen euro, één pond, tien dollar |
| `21 °C`, `1 °C` | vingt et un degrés Celsius, un degré | einundzwanzig Grad Celsius, ein Grad | ventuno gradi Celsius, un grado | eenentwintig graden Celsius, één graad |
| `#5`, `n° 5`, `@`, `&`, `10-20` | numéro cinq, arobase, et, dix à vingt | Nummer fünf, at, und, zehn bis zwanzig | numero cinque (`n. 5` too), chiocciola, e, dieci a venti | nummer vijf, at, en, tien tot twintig |

**Abbreviations** (new with these four; written as they are, at the start of a word): French `M.` and `MM.` (only
before a capitalized name), `Mme`, `Mlle`, `Dr`, `etc.`, `p. ex.`, `c.-à-d.`, `env.`, `av. J.-C.`; German `z. B.`,
`d. h.`, `u. a.`, `usw.`, `bzw.`, `ca.`, `Nr.`, `Dr.`, `Prof.`, `evtl.`, `ggf.`, `inkl.`, `vgl.`, `bzgl.`, `Str.`;
Italian `Sig.`, `Sig.ra`, `Dott.`, `Dott.ssa`, `Prof.`, `ecc.`, `pag.`, `p.es.`, `ad es.`, `tel.`; Dutch `bijv.`,
`d.w.z.`, `enz.`, `o.a.`, `m.b.t.`, `i.p.v.`, `nr.`, `ca.`, `dhr.`, `mevr.`, `blz.`, `incl.`, `t/m`. One that can end
a sentence (`etc.`, `usw.`, `ecc.`, `enz.`) keeps its period for it when a capital or the end of the text follows.

French's elisions keep their apostrophe: `l'11e` is l'onzième. Not covered, besides what Spanish leaves out: Roman
numerals (`XXIe siècle` stays), Belgian and Swiss French, German ordinals without an article or a month (`Er wurde 3.`
is "drei", as a sentence's end can't be told from an ordinal), the declension of German ordinals beyond -e, -en and
-er, and grammatical gender before a noun (`1 maison` is "un maison").

Which language a text is in (`say_language`) now weighs seven lists of common words and the letters only one or two of
them use (ß ä ö for German, ì ò for Italian, ê î û œ for French, ç for French and Portuguese); the curly apostrophe
splits French words as `'` does. Without `-l`, a text too short to tell goes on being read in the person's interface
language when it is one of the seven.

Tests: 274 new checks in `saytext_test.c` (541 in all): French 86, German 70, Italian 59 and Dutch 46 for normalization,
13 for telling the languages apart (Spanish and Portuguese sentences full of numbers stay Spanish and Portuguese).
`tts_test.sh` says a sentence with numbers, a time and a date in each new language, HD and standard, where the models
are installed.

**Listen-free check, 2026-11-22**, on an i9-10900 with an RX 580: every new model (HD and standard), with Estelle and
Alba, said a sentence of 10–13 s with a number, a time, a date and a price, on the CPU and on the GPU, and the WAV was
measured: voiced 45–82 % of the time, spectral flatness 0.000–0.001 where speech is (white noise: 0.56), 89–100 % of the
energy in 80 Hz–4 kHz, and 4.4–5.9 syllables a second of the normalized text, the same as the Spanish models with the
same sentence (4.6–5.8). Synthesis took 1.5–2.1 s on the GPU and 3.1–4.3 s on the CPU with a standard model, 5.9–6.6 s
and 8.7–11.9 s with an HD one (model loading included). A transcription round trip (whisper.cpp) as above is still to
be done on the maintainer's machine.

## Reading aloud from the desktop

Every place with text has **Speech** (`sdk/voice.js`, `speechMenu`):

- **Edit → Speech** (Text Editor, Terminal; Notes in View) and the **right-click** menu (Text Editor, Terminal, Notes):
  *Start Speaking*, *Stop Speaking*, *Save as Audio…* Start Speaking reads the selection, or the whole text when
  nothing is selected (Text Editor, Notes). The right-click menu is the SDK's (`sdk/contextmenu.jsx`, with submenus).
- **A Linux app** (Firefox, foot…) gets an **Edit** menu with Speech in the menu bar (Classic: in its window's
  menu strip), as its own menus aren't the desktop's (`useMenusWithSpeech`). It reads the app's **primary selection**.
- **A web app** (`lord-web`, in development) is such a Linux app: it makes the page's selection the primary selection
  (never a password field's), so Edit → Speech, ❖Esc and its right-click menu's Speech ▸ read it the same way
  (`docs/WEBAPPS.md`, "Editing"). With nothing selected there is nothing to read: the page's whole text is not read.
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

| Quality | What | Size |
|---|---|---|
| Standard | the 6-layer model, audio.cpp's Q8_0 GGUF: fast anywhere. Dutch: Kyutai's own 6-layer folder (`dutch`), as audio.cpp publishes no Dutch GGUF | 128 MB (French 129, Dutch 220) |
| HD | Kyutai's 24-layer model (`spanish_24l`, `portuguese_24l`, `english_2026-09_24l`, `french_24l`, `german_24l`, `italian_24l`, `dutch_24l`), read as Kyutai ships it: more natural | 672 MB (English 1304) |

**23 voices** (`sdk/voicecatalog.js`), each speaking every language: Kyutai's prepared voice states (21–31 MB in HD,
4–8 MB standard). Only voices whose recording allows redistribution and commercial use: Kyutai's own (lola, juergen,
rafael, estelle, giovanni, daan), voice-zero (CC0), Alba McKenna and VCTK (CC-BY-4.0). Left out: jean (EARS,
non-commercial) and cosette, javert, marius (origin not documented). Daan exists only in HD, but in French (audio.cpp's
GGUF has it) and Dutch (Kyutai's own folder). Each language's own voice is the default: Lola (es), Rafael (pt), Alba
(en), and Kyutai's voice of each new language, Estelle (fr), Juergen (de), Giovanni (it) and Daan (nl).

**Licenses of the new languages** (checked on 2026-11-22): Kyutai's `french_24l`, `german_24l`, `italian_24l`,
`dutch_24l` and `dutch` are in the same `kyutai/pocket-tts` repository, under the same CC-BY-4.0 model card and the
same gated conditions as the three before; audio.cpp's French, German and Italian Q8_0 GGUFs are in
`audio-cpp/audio.cpp-gguf` as the Spanish one (conversions of Kyutai's weights, CC-BY-4.0). The voices are the same 23
recordings, so their licenses don't change.

**Packages** (`tools/voice/mkdeb-voice.mjs`, `Architecture: all`, into `/usr/share/lordos/voice`), published in the
signed APT repository, never fetched from Hugging Face by a LordOS system:

- `lordos-voice-<lang>` and `lordos-voice-<lang>-hd`: the models;
- `lordos-voice-<lang>-<voice>` and `…-hd`: one voice, depending on its model;
- `lordos-voice-previews`: a recorded sample of every voice in every language (45 MB with the seven languages), so a voice is heard before it is
  downloaded (`say --preview`, `lord.voice.preview`).

Building them, on the maintainer's machine: `tools/voice/fetch-voices.sh <dir>` (Kyutai's gated repository needs its
conditions accepted and a read token; each file checked against the SHA-256 Hugging Face publishes, and a file it
publishes none for is refused), then
`tools/voice/make-previews.sh <dir>`, then `node tools/voice/mkdeb-voice.mjs <dir> <out> <YYYY.MM.DD>`. The packages
are reproducible; `tools/release/apt-repo.sh build … <the .debs>` publishes them like any other. With the seven
languages it is 332 packages (14 models, 161 HD voices, 156 standard voices, the previews); the three steps were run
end to end on 2026-11-22 on the development PC (every file matched Hugging Face's SHA-256; 161 samples), the packages
left unsigned and unpublished.

**What lord-tts does like Kyutai's own code** (`pocket_tts`, checked against it): temperature 0.3 for every model
(audio.cpp defaults to 0.7 outside English, which made Spanish drag and stumble), and the model's character
replacements before synthesis (quotes, brackets and, in Spanish, `¡ ¿` dropped; curly apostrophes to `'`; a dash to a
comma, in German the em dash too; in French `:` to a comma, and in French and German `;` to a comma). The model starts every text with 0.3–0.9 s of silence (Kyutai's code too); `say` cuts it from the first
sentence, keeping 50 ms.

## Settings → AI → Voice

(0.8.9.6.2) The Voice page is now the **Voice capability** of the AI store ([AI.md](AI.md)): the store shows one card
per language (its standard model and own voice: install standard or HD, remove, listen to the recorded sample, try),
and the capability's page ("‹ Artificial intelligence › Voice") holds everything below, the models and voices as cards
with a sheet each. Nothing was taken away.

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
`portuguese_24l`, `english_2026-09_24l`, `french_24l`, `german_24l`, `italian_24l`, `dutch_24l` and `dutch`, the
Dutch standard model), `file` is a model's folder, read with `lib/tts/pocket_tts_dir.json`
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

Tests: `server/tests/saytext_test.c` (cleaning terminal output, splitting sentences, telling the language, writing
numbers, symbols and abbreviations out as words in every language but English),
`server/tests/levels_test.c` (the wave's bands), `server/tests/voiceplayer_model_test.mjs` (where the player goes, the
wave's motion; all three in `make test`) and
`server/tests/tts_test.sh` (speech in es/en/pt, and fr/de/it/nl, HD and standard, where installed, on the CPU and the
GPU, the confinement in both, a voice file that isn't audio, a voice name that is a path; `make test-full` when a model
is installed).

Not yet tested: the Raspberry Pi (aarch64: ggml's CPU variants and the Pi 5's Vulkan driver).

## Credits

Pocket TTS © Kyutai, CC-BY-4.0 (https://huggingface.co/kyutai/pocket-tts), converted to GGUF by audio.cpp. Kyutai
forbids cloning a voice without the person's explicit consent, impersonation and presenting generated speech as a real
recording; LordOS shows that notice where voices are created.
