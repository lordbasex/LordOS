## The short version

**In development for 0.8.8.6; not released yet. The latest release is 0.8.8.5.**

LordOS reads text aloud on your own computer, without the internet. Select something in the Text Editor, the Terminal, Notes or a Linux app such as Firefox, press **❖Esc**, and you hear it. Press ❖Esc again and it stops. Nothing you read is sent anywhere: the speech is made on your processor, or on your graphics card when it has one that helps.

Think of a reader who sits next to you and only ever reads what you point at. The reader doesn't take notes, doesn't make phone calls and doesn't leave the room.

[[diagram:flow]]

## Where you find it

- **Speech in every app with text.** *Edit → Speech* in the Text Editor and the Terminal (in Notes, under View), and the same in the right-click menu of the Text Editor, the Terminal and Notes: *Start Speaking*, *Stop Speaking* and *Save as Audio…*. Start Speaking reads the selection, or the whole text when nothing is selected.
- **Linux apps too.** A Linux app gets an *Edit* menu with Speech in the menu bar (in Lord Classic, in its window's menu strip), which reads what you selected in it.
- **A small player.** While it reads, a pill shows the voice's wave in color, with read again, pause and go on, stop, and close. You can drag it anywhere, even to another display, and the next reading opens there.
- **Save as Audio…** asks where to save with the usual dialog and writes a WAV file.
- **Live Speech** (❖⇧Esc): a box where you type, press Enter, and it is said with your chosen voice. Recent phrases stay below, in memory only, to be said again. It helps anyone who can't or would rather not speak.
- **The `say` command** in a terminal: `say "Hello"`, `echo text | say`, `say -f notes.txt -o notes.wav`, `say -v <voice>`, `say --voices`.

The language (Spanish, English or Portuguese) is told from the text itself; when it can't be, the interface's language is used.

## Voices you choose

**No voice comes with LordOS.** In **Settings → AI → Voice** you download only what you want, per language:

- a **Standard** model (128 MB), fast on any computer, and an **HD** one (672 MB; English 1.3 GB), more natural;
- **23 voices**, 11 female and 12 male, each speaking every language. **▶** plays a recorded sample before you download anything.

They come as **signed LordOS packages** from LordOS's own package repository, never from a third-party website. Only voices whose recordings allow redistribution and commercial use are offered; voices with a non-commercial license or an undocumented origin were left out.

## Your own voice

**Personal Voice** makes a voice from yours in about a minute, and it speaks every language:

1. LordOS explains what it is and asks for your consent before anything is recorded.
2. It listens to the room for three seconds first. If it's too noisy, or the microphone sends nothing, it says so.
3. You read six short phrases, one at a time (each stops by itself when you finish), or you **speak freely** for 15 to 30 seconds about anything, for anyone who finds reading aloud hard.

The recording stays in your account, on this computer. **Use a recording…** does the same from an audio file (WAV, MP3, M4A, OGG, Opus or FLAC).

## How it protects you

- **The engine is locked in.** Speech is made by `lord-tts`, a small helper that runs in LordOS's helper sandbox: no network, no sockets, no starting programs, and it can write only to its own cache folder. It is the only program that links the speech libraries. When it uses the graphics card, it may open only the card's render node, never the display that `lord-server` drives.
- **Your text never goes in a command line.** `lord-server` starts `lord-say` from a fixed path, with fixed arguments, and gives it the text as a sealed memory file. The only thing the interface can choose is a voice name, checked in C.
- **Saving can't be pointed elsewhere.** `lord-server` checks the folder you chose, creates the file there and renames it when the audio is done, so `say` never sees a path.
- **A Linux app's selection stays with it.** The desktop reads it only while that app has the keyboard, and it never reaches JavaScript.
- **Isolated apps can't use it.** Installed `.lpk` apps have no voice API.

## Speed

Measured on one PC, an Intel i9-10900 with a Radeon RX 580: about 10 seconds of speech in 1.3 s on the graphics card and 3 s on the processor, with about 220 MB of memory. It reads a sentence at a time, so the first one plays while the next is being made. A sentence that comes out with a hiss under the voice is caught by measuring it and made again.

## Honest limits

- **Not released yet.** All of this is in development for 0.8.8.6.
- **Three languages:** Spanish, English and Portuguese.
- **You download a model first.** Until then, Speech says that no voice is installed.
- **Not tested on the Raspberry Pi** (its processor's code paths and the Pi 5's graphics driver).
- **It reads what you select; it is not a screen reader.** A screen reader, zoom and high contrast are still on the roadmap.

## Read the source

- [docs/VOICE.md](https://github.com/lordbasex/LordOS/blob/main/docs/VOICE.md): the design, models, voices and tests
- [server/src/doc/h_tts.c](https://github.com/lordbasex/LordOS/blob/main/server/src/doc/h_tts.c): `lord-tts`, the sandboxed engine
- [server/src/tts/speak.c](https://github.com/lordbasex/LordOS/blob/main/server/src/tts/speak.c): how `lord-server` starts `lord-say`
- [server/src/ui/js_voice.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_voice.c): the `lord.voice` API
- [apps/settings/PersonalVoice.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/settings/PersonalVoice.jsx) and [sdk/livespeech.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/livespeech.jsx): Personal Voice and Live Speech

Speech comes from Kyutai's Pocket TTS (model weights under CC-BY-4.0), run by audio.cpp (Apache-2.0) and its ggml (MIT).
