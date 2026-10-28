#!/bin/sh
# run_suites.sh — runs the test programs that `make test` builds but whose own rules don't run
# them (the other suites run inside their build rule). Called from server/ by the Makefile:
#
#   make -C server test         sh tests/run_suites.sh        self-contained suites, a few seconds
#   make -C server test-full    sh tests/run_suites.sh full   also the helpers (lord-media, lord-doc),
#                                                             the sandbox and the external comparisons
#
# Exits with 1 if any suite fails. A suite whose tool or data isn't installed is reported as
# "skipped: needs X" and doesn't fail the run. Optional data: BIDI_TEST=<BidiCharacterTest.txt>.
cd "$(dirname "$0")/.." || exit 1
mode=${1:-quick}
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-suites.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
trap 'exit 130' INT TERM
pass=0 fails=0 skips=0 failed=''

ok()   { pass=$((pass + 1)); printf '  ok       %s\n' "$1"; }
skip() { skips=$((skips + 1)); printf '  skipped  %s: needs %s\n' "$1" "$2"; }
bad()  {                                # NAME [file with the details; default: the output]
    fails=$((fails + 1)); failed="$failed${failed:+; }$1"
    printf '  FAILED   %s\n' "$1"
    tail -n 30 "${2:-$T/out}" | sed 's/^/           | /'
}
# run NAME CMD...: passes when CMD exits with 0
run() { n=$1; shift; if "$@" >"$T/out" 2>&1; then ok "$n"; else bad "$n"; fi; }
# has_all FILE: every line of $T/want appears in FILE
has_all() { while IFS= read -r l; do grep -qF -- "$l" "$1" || return 1; done <"$T/want"; }
have() { command -v "$1" >/dev/null 2>&1; }

# vt NAME INPUT COLS ROWS [COLS2 ROWS2 [SEARCH]] <<EOF (exact expected grid) — terminal emulator.
# INPUT is a printf format (octal escapes: /bin/sh may be dash).
vt() {
    n=$1 in=$2; shift 2
    cat >"$T/want"
    # shellcheck disable=SC2059
    printf "$in" | build/vt-dump "$@" >"$T/out" 2>&1
    if cmp -s "$T/want" "$T/out"; then ok "$n"; else diff "$T/want" "$T/out" >"$T/diff"; bad "$n" "$T/diff"; fi
}

# ime NAME ENGINE KEYS DICT <<EOF (lines that must appear) — input methods; DICT is what to install
# when the engine needs a dictionary ("" for none). Without it, ime-test prints an empty «».
ime() {
    n=$1 eng=$2 keys=$3 dict=$4
    cat >"$T/want"
    if ! build/ime-test "$eng" "$keys" >"$T/out" 2>&1; then bad "$n"; return; fi
    if [ -n "$dict" ] && grep -qF '«»' "$T/out"; then skip "$n" "$dict"; return; fi
    if has_all "$T/out"; then ok "$n"; else bad "$n"; fi
}

# shape NAME FONT MODE ARGS... <<EOF (exact glyphs) — OpenType shaping of the bundled fonts, against
# the output of the last checked build (regression; the HarfBuzz comparison is hb_compare.py)
shape() {
    n=$1; shift
    cat >"$T/want"
    build/shape-test "$@" >"$T/out" 2>&1
    if cmp -s "$T/want" "$T/out"; then ok "$n"; else diff "$T/want" "$T/out" >"$T/diff"; bad "$n" "$T/diff"; fi
}

quick() {
    echo "== suites (tests/run_suites.sh)"
    # terminal emulator (examples of docs/TERMINAL.md and docs/VM-SETUP.md)
    vt "vt-dump: colors, resize and search" 'hello\033[31m red\r\n' 20 5 12 6 red <<'EOF'
--- 20x5 cursor 0,1 history 0 alt 0
   |hello red|
   ||
   ||
   ||
   ||
--- 12x6 cursor 0,1 history 0 alt 0
   |hello red|
   ||
   ||
   ||
   ||
   ||
search 'red': 1 matches
EOF
    # (the expected grid has "e" + U+0301, as the emulator keeps it)
    vt "vt-dump: combining mark and flag (mode 2027)" 'e\314\201\033[?2027h\360\237\207\246\360\237\207\267' 10 2 <<'EOF'
--- 10x2 cursor 3,0 history 0 alt 0
   |[é][🇦🇷]|
   ||
EOF
    vt "vt-dump: reflow with wide characters" '123456789日本語\r\n' 10 3 7 3 <<'EOF'
--- 10x3 cursor 0,2 history 0 alt 0
  +|123456789|
   |日本語|
   ||
--- 7x3 cursor 0,2 history 1 alt 0
h +|1234567|
  +|89日本|
   |語|
   ||
EOF

    shape "shape-test: Inter kerning (GPOS)" ../assets/fonts/Inter-Regular.ttf ui latn 0 AVATAR <<'EOF'
2@0,0|456@1273,0|2@2546,0|411@3785,0|2@4933,0|384@6346,0
EOF
    shape "shape-test: JetBrains Mono ligatures (GSUB)" ../assets/fonts/JetBrainsMono-Regular.ttf liga 'a->b != c' <<'EOF'
189|1742|881|214|958|1742|912|958|215
EOF

    ime "ime-test: hangul" hangul gksrmf "" <<'EOF'
committed: 한
preedit: 글
EOF
    ime "ime-test: kana" kana toukyou_ "skkdic" <<'EOF'
preedit: 東京
EOF
    ime "ime-test: pinyin" pinyin nihao_ "rime-data-pinyin-simp" <<'EOF'
committed: 你好
EOF

    # bidi: the Unicode conformance file isn't in the repository
    bidi=${BIDI_TEST:-}
    for f in /usr/share/unicode/BidiCharacterTest.txt /usr/share/unicode-data/BidiCharacterTest.txt tests/BidiCharacterTest.txt; do
        [ -z "$bidi" ] && [ -r "$f" ] && bidi=$f
    done
    if [ -n "$bidi" ] && [ -r "$bidi" ]; then run "bidi-test: $(basename "$bidi")" build/bidi-test "$bidi"
    else skip "bidi-test" "BidiCharacterTest.txt (unicode-data, or BIDI_TEST=<file>)"; fi

    # Thai word breaks: the dictionary is libthai-data's; the comparison needs libthai itself
    if [ ! -r /usr/share/libthai/thbrk.tri ]; then skip "thbrk-test" "libthai-data (/usr/share/libthai/thbrk.tri)"
    elif pkg-config --exists libthai 2>/dev/null && have python3; then run "thbrk-test against libthai" sh tests/thbrk_compare.sh
    else
        run "thbrk-test (dictionary)" sh -c 'build/thbrk-test < tests/th-break.txt'
        skip "thbrk_compare.sh" "libthai-dev and python3"
    fi

    run "sound-test" build/sound-test
    # WebAssembly modules: built by tests/wasm/build.sh (*.wasm is not committed)
    if [ -f tests/wasm/demo.wasm ] && [ -f tests/wasm/api.wasm ] || { have clang && have wasm-ld && have wat2wasm && sh tests/wasm/build.sh >"$T/out" 2>&1; }; then
        run "wasm-test" build/wasm-test
        run "wasm-js-test" build/wasm-js-test
    else
        skip "wasm-test, wasm-js-test" "clang, lld (wasm-ld) and wabt (wat2wasm) to build tests/wasm"
    fi
    run "net-test crypto" build/net-test crypto
    run "net-test errors" build/net-test errors
    run "pkg-test" build/pkg-test
    run "fsops-test" build/fsops-test
    run "sign-test" build/sign-test
    run "mmdb-test" build/mmdb-test
    run "canvas-test" build/canvas-test
    run "arcpath-test" build/arcpath-test
    run "ipp-test" build/ipp-test
    run "tp-test" build/tp-test
    run "fan-test" build/fan-test
    # the firewall rule sheet's parser (sdk/fwrule.js) against the same cases as build/fwdest-test
    if have node; then run "fwrule-input-test (sdk/fwrule.js)" node tests/fwrule_input_test.mjs
    else skip "fwrule-input-test" "node"; fi
    # the Dock's pinned-list model (sdk/dockmodel.js)
    if have node; then run "dock-model-test (sdk/dockmodel.js)" node tests/dock_model_test.mjs
    else skip "dock-model-test" "node"; fi
    # window snapping geometry (sdk/snap.js): zones without gaps or overlaps, drop zones, Super+arrows
    if have node; then run "snap-test (sdk/snap.js)" node tests/snap_test.mjs
    else skip "snap-test" "node"; fi
    # smartctl's JSON → the Disks/Monitor SMART model (sdk/smartparse.js), with real smartctl output
    if have node; then run "smart-parse-test (sdk/smartparse.js)" node tests/smart_parse_test.mjs
    else skip "smart-parse-test" "node"; fi
    # the Store's "Suggested for your computer" rules (assets/store/suggestions.json)
    if have node; then run "suggest-engine-test (sdk/suggestengine.js)" node tests/suggest_engine_test.mjs
    else skip "suggest-engine-test" "node"; fi
    # the LordOS Imager's shared logic (apps/imager/core) and its first-run files checked by the C parser
    if have node; then run "imager-core-test (apps/imager/core)" node tests/imager_core_test.mjs
    else skip "imager-core-test" "node"; fi
    # the overlay scroll bar's model (sdk/scrollbar.js): geometry, thumb hit-test and drag, fade timing
    if have node; then run "scrollbar-model-test (sdk/scrollbar.js)" node tests/scrollbar_model_test.mjs
    else skip "scrollbar-model-test" "node"; fi
    # (0.8.5) Monitor → Network's model (sdk/netmodel.js): search tokens, filters, the tree, sorting
    if have node; then run "netmodel-test (sdk/netmodel.js)" node tests/netmodel_test.mjs
    else skip "netmodel-test" "node"; fi
    # (0.8.5) System Information's model (apps/sysinfo/model.js): GPU layers, CPU reasons, Compatibility
    if have node; then run "sysinfo-model-test (apps/sysinfo/model.js)" node tests/sysinfo_model_test.mjs
    else skip "sysinfo-model-test" "node"; fi
    # (0.8.6) LordBench's model (apps/sysinfo/perf.js): references, verdicts, comparisons
    if have node; then run "sysinfo-perf-test (apps/sysinfo/perf.js)" node tests/sysinfo_perf_test.mjs
    else skip "sysinfo-perf-test" "node"; fi
    # (0.8.5) Settings → Displays' arrangement (sdk/displaylayout.js): numbers, magnetic snap, valid drops
    if have node; then run "display-layout-test (sdk/displaylayout.js)" node tests/display_layout_test.mjs
    else skip "display-layout-test" "node"; fi
    # (0.8.6) runtime/index.js's skip of unchanged styles sends the same lord.* calls as without it
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "runtime-props-test (runtime/index.js)" node tests/runtime_props_test.mjs
    else skip "runtime-props-test" "node and the repository's node_modules (npm install)"; fi
}

full() {
    echo "== slow and environment-dependent suites (tests/run_suites.sh full)"
    # media helper: a generated WAV (one second of a 440 Hz tone), probe, waveform and its confinement
    if [ -x build/lord-media ] && have python3; then
        python3 -I - "$T/tone.wav" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 440 * i / 8000))) for i in range(8000)))
EOF
        run "media-test probe" build/media-test probe "$T/tone.wav"
        run "media-test wave" build/media-test wave "$T/tone.wav" 50
        run "media-test sandbox" build/media-test sandbox "$T/tone.wav"
    else skip "media-test" "build/lord-media (FFmpeg -dev packages) and python3"; fi
    # document helper: malicious archives and the confinement
    if [ -x build/lord-doc ] && [ -x build/doc-test ] && have python3; then run "doc_evil.sh (lord-doc)" sh tests/doc_evil.sh
    else skip "doc_evil.sh" "build/lord-doc (Poppler and libarchive -dev packages) and python3"; fi
    # sandbox: Landlock and seccomp of the running kernel
    if [ -r /sys/kernel/security/lsm ] && grep -q landlock /sys/kernel/security/lsm; then
        run "sandbox-test (app)" build/sandbox-test
        run "sandbox-test (app, net)" build/sandbox-test net
        run "sandbox-test (helper)" build/sandbox-test helper
    else skip "sandbox-test" "a kernel with Landlock enabled (run it in the VM)"; fi
    # shaping against HarfBuzz, with the system's Noto fonts and the bundled ones
    if have python3 && python3 -c 'import uharfbuzz' 2>/dev/null; then
        mkdir -p "$T/fonts"
        find /usr/share/fonts ../assets/fonts -name '*.tt[fc]' 2>/dev/null | while IFS= read -r f; do
            ln -sf "$(cd "$(dirname "$f")" && pwd)/$(basename "$f")" "$T/fonts/$(basename "$f")"; done
        run "hb_compare.py (HarfBuzz)" python3 tests/hb_compare.py "$T/fonts" build/shape-test
    else skip "hb_compare.py" "python3 with uharfbuzz (pip install uharfbuzz) and fonts-noto-core/fonts-noto-cjk"; fi
}

case $mode in
    quick) quick ;;
    full) full ;;
    *) echo "usage: sh tests/run_suites.sh [full]" >&2; exit 2 ;;
esac
printf '== %d passed, %d failed, %d skipped\n' "$pass" "$fails" "$skips"
[ "$fails" -eq 0 ] || { echo "failed: $failed"; exit 1; }
