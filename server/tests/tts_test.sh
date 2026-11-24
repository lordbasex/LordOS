#!/bin/sh
# tts_test.sh — lord-tts and say (docs/VOICE.md): speech in Spanish, English and Portuguese on the CPU (and the GPU when
# there is a render node), what lord-tts's confinement blocks in both, a voice file that isn't audio, and terminal
# colors in the text; French, German, Italian and Dutch, HD and standard, where their models are installed (in either
# folder). Needs build/lord-tts, build/lord-say, build/doc-test and the models in
# ~/.local/share/lordos/voice/models/<es|en|pt>/standard.gguf (or /usr/share/lordos/voice).
cd "$(dirname "$0")/.." || exit 2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
M=$HOME/.local/share/lordos/voice/models
[ -f "$M/es/standard.gguf" ] || M=/usr/share/lordos/voice/models
# seconds of sound (not silence) in a 16-bit mono WAV
voiced() { python3 -I -c "
import sys, wave, array
w = wave.open(sys.argv[1]); a = array.array('h', w.readframes(w.getnframes())); r = w.getframerate()
loud = sum(1 for i in range(0, len(a), r // 50) if max(abs(x) for x in a[i:i + r // 50] or [0]) > 1000)
print(round(loud / 50, 1))" "$1"; }
mkdir -p "$T/home/.local/share/lordos/voice"
ln -s "$M" "$T/home/.local/share/lordos/voice/models"
# a real voice to clone, made here: the English model's built-in voice (alba, CC-BY-4.0)
VOICE=$T/voice.wav
HOME=$T/home build/lord-say -b cpu -l en -o "$VOICE" "This is a sample of my voice, recorded for LordOS to learn how I speak." 2>/dev/null
check "a reference voice was made" '[ "$(voiced "$VOICE" | cut -d. -f1)" -ge 3 ]'
mkdir -p "$T/home/.local/share/lordos/voice/voices"
cp "$VOICE" "$T/home/.local/share/lordos/voice/voices/test.wav"
for b in cpu gpu; do
    [ "$b" = gpu ] && [ ! -e /dev/dri/renderD128 ] && { echo "skip GPU: no render node"; continue; }
    HOME=$T/home build/lord-say -b $b -l es -v test -o "$T/es-$b.wav" "Hola, esta es la voz de LordOS. Funciona sin internet." 2>"$T/es-$b.err"
    check "$b: Spanish speech" '[ "$(voiced "$T/es-$b.wav" | cut -d. -f1)" -ge 2 ]'
    build/doc-test build/lord-tts selftest-tts "$M" $b > "$T/self-$b.out" 2>/dev/null
    check "$b: cannot read ~/.ssh or the home folder" 'grep -q "\"ssh\":[1-9]" "$T/self-$b.out" && grep -q "\"home_file\":[1-9]" "$T/self-$b.out"'
    check "$b: cannot write /tmp or the models" 'grep -q "\"write_tmp\":[1-9]" "$T/self-$b.out" && grep -q "\"write_models\":[1-9]" "$T/self-$b.out"'
    check "$b: no network, sockets or fork" 'grep -q "\"inet\":[1-9]" "$T/self-$b.out" && grep -q "\"unix_socket\":[1-9]" "$T/self-$b.out" && grep -q "\"fork\":[1-9]" "$T/self-$b.out"'
    check "$b: never the display's node (card0)" 'grep -q "\"card0\":[1-9]" "$T/self-$b.out"'
done
check "GPU: the render node opens; CPU: it doesn't" '[ ! -e /dev/dri/renderD128 ] || { grep -q "\"render_ok\":0" "$T/self-gpu.out" && grep -q "\"render_ok\":[1-9]" "$T/self-cpu.out"; }'
HOME=$T/home build/lord-say -b cpu -l en -o "$T/en.wav" "Hello, this is the voice of LordOS." 2>/dev/null
check "English with its built-in voice" '[ "$(voiced "$T/en.wav" | cut -d. -f1)" -ge 1 ]'
HOME=$T/home build/lord-say -b cpu -l pt -v test -o "$T/pt.wav" "Olá, esta é a voz do LordOS." 2>/dev/null
check "Portuguese" '[ "$(voiced "$T/pt.wav" | cut -d. -f1)" -ge 1 ]'
# French, German, Italian and Dutch, each where its model is installed (the person's folder or the system's), with its
# own voice; numbers and a date in each, written out as words by say (saytext.c)
for L in "fr:Bonjour, j'ai 88 ans et la réunion est à 14:30 le 15/11/2026." \
         "de:Hallo, ich bin 88 Jahre alt und das Treffen ist am 15.11.2026 um 14:30." \
         "it:Ciao, ho 88 anni e la riunione è alle 14:30 del 15/11/2026." \
         "nl:Hallo, ik ben 88 jaar en de vergadering is om 14:30 op 15-11-2026."; do
    l=${L%%:*}; text=${L#*:}
    R=$HOME/.local/share/lordos/voice/models
    [ -d "$R/$l" ] || R=/usr/share/lordos/voice/models
    [ -d "$R/$l" ] || { echo "skip $l: no model"; continue; }
    mkdir -p "$T/h-$l/.local/share/lordos/voice"
    ln -s "$R" "$T/h-$l/.local/share/lordos/voice/models"
    for q in hd standard; do
        [ -f "$R/$l/hd/model.safetensors" ] || [ "$q" = standard ] || continue
        [ -f "$R/$l/standard.gguf" ] || [ -f "$R/$l/standard/model.safetensors" ] || [ "$q" = hd ] || continue
        HOME=$T/h-$l build/lord-say -b cpu -l $l -q $q -o "$T/$l-$q.wav" "$text" 2>/dev/null
        check "$l ($q): speech" '[ "$(voiced "$T/$l-$q.wav" | cut -d. -f1)" -ge 3 ]'
    done
done
printf '\033[1;32mlordbasex\033[0m dice: hola.\n' | HOME=$T/home build/lord-say -b cpu -l es -v test -o "$T/ansi.wav" 2>/dev/null
check "terminal colors in the text are not read" '[ -s "$T/ansi.wav" ]'
HOME=$T/home build/lord-say "Hola." -o "$T/order.wav" -l es -v test -b cpu 2>/dev/null
check "options after the text: written, not played" '[ -s "$T/order.wav" ]'
printf 'not a wav at all, just text that pretends to be one' > "$T/home/.local/share/lordos/voice/voices/bad.wav"
HOME=$T/home build/lord-say -b cpu -l es -v bad -o "$T/bad.wav" "Hola." 2>"$T/bad.err"
check "a voice that isn't audio is refused" 'grep -q "usable WAV" "$T/bad.err"'
HOME=$T/home build/lord-say -b cpu -l es -v ../../etc/passwd "Hola." 2>"$T/path.err"
check "a voice name can't be a path" 'grep -q "letters, digits" "$T/path.err"'
[ "$fails" = 0 ] && echo "tts_test: all passed" || echo "tts_test: $fails failed"
[ "$fails" = 0 ]
