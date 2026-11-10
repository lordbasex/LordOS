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
    run "net-test ifwatch" build/net-test ifwatch
    run "net-test wake" build/net-test wake
    run "pkg-test" build/pkg-test
    run "fsops-test" build/fsops-test
    run "fsasync-test" build/fsasync-test
    run "sign-test" build/sign-test
    run "mmdb-test" build/mmdb-test
    run "canvas-test" build/canvas-test
    run "imgops-test" build/imgops-test
    run "arcpath-test" build/arcpath-test
    run "ipp-test" build/ipp-test
    run "tp-test" build/tp-test
    run "fan-test" build/fan-test
    run "freespace-test" build/freespace-test
    run "install-plan-test" build/install-plan-test
    # the firewall rule sheet's parser (sdk/fwrule.js) against the same cases as build/fwdest-test
    if have node; then run "remotecert-test (sdk/remotecert.js)" node tests/remotecert_test.mjs; fi
    if have node; then run "remotedesk-test (sdk/remotedeskmodel.js)" node tests/remotedesk_test.mjs; fi
    if have node; then run "place-model-test (sdk/placemodel.js)" node tests/place_model_test.mjs; fi
    if have node; then run "statusplace-test (sdk/statusplace.js)" node tests/statusplace_test.mjs; fi
    if have node; then run "menubar-model-test (sdk/menubarmodel.js, sdk/ticker.js)" node tests/menubar_model_test.mjs; fi
    if have node; then run "editor-scrollbar-test (apps/editor/scrollbar.js)" node tests/editor_scrollbar_test.mjs; fi
    if have node; then run "photos-places-test (apps/photos/places.js)" node tests/photos_places_test.mjs; fi
    if have node; then run "bt-power-model-test (sdk/btpowermodel.js)" node tests/bt_power_model_test.mjs; fi
    if have node; then run "bt-audio-model-test (sdk/btaudiomodel.js)" node tests/bt_audio_model_test.mjs; fi
    if have node; then run "bt-list-model-test (sdk/btlistmodel.js)" node tests/bt_list_model_test.mjs; fi
    if have node; then run "windowbuttons-test (sdk/windowbuttons.js)" node tests/windowbuttons_test.mjs; fi
    if have node; then run "login-model-test (sdk/loginmodel.js)" node tests/login_model_test.mjs; fi
    if have node; then run "hwkeys-test (sdk/hwkeymodel.js)" node ../tests/hwkeys_test.mjs; fi
    if have node; then run "fwrule-input-test (sdk/fwrule.js)" node tests/fwrule_input_test.mjs
    else skip "fwrule-input-test" "node"; fi
    # which installed kernels Cleaner may offer (sdk/oldkernels.js)
    if have node; then run "oldkernels-test (sdk/oldkernels.js)" node tests/oldkernels_test.mjs
    else skip "oldkernels-test" "node"; fi
    # the Dock's pinned-list model (sdk/dockmodel.js)
    if have node; then run "files-model-test (apps/files)" node tests/files_model_test.mjs; fi
    if have node; then run "files-remote-test (apps/files/remotedirs.js)" node tests/files_remote_test.mjs; fi
    if have node; then run "textfield-model-test (sdk/textfieldmodel.js)" node tests/textfield_model_test.mjs; fi
    if have node; then run "textarea-model-test (sdk/textareamodel.js)" node tests/textarea_model_test.mjs; fi
    if have node; then run "filekinds-test (sdk/filekinds.js)" node tests/filekinds_test.mjs; fi
    if have node; then run "thumb-cache-test (sdk/thumbcache.js)" node tests/thumb_cache_test.mjs; fi
    # Cloud without a screen: status glyphs, space, providers, menus, sidebar, menu bar, notifications, the mock's shape (sdk/cloudmodel.js)
    if have node; then run "cloud-model-test (sdk/cloudmodel.js)" node tests/cloud_model_test.mjs
    else skip "cloud-model-test" "node"; fi
    # a browser sign-in against a stand-in rclone: a second account, a drive that can't be opened, none (#87)
    if have python3 && [ -x build/cloud-harness ]; then run "cloud-signin-fake (src/cloud, OneDrive's drive choice)" sh tests/cloud_signin_fake.sh build
    else skip "cloud-signin-fake" "python3 and build/cloud-harness"; fi
    if have node; then run "mail-foldertree-test (apps/mail)" node tests/mail_foldertree_test.mjs; fi
    if have node; then run "mail-auth-model-test (sdk/mailauthmodel.js)" node tests/mail_auth_model_test.mjs; fi
    if have node; then run "balance-model-test (apps/settings/balance.js)" node tests/balance_model_test.mjs; fi
    # the lock around a system sleep (sdk/idlemodel.js, issue #36)
    if have node; then run "idle-model-test (sdk/idlemodel.js)" node tests/idle_model_test.mjs
    else skip "idle-model-test (sdk/idlemodel.js)" node; fi
    if have node; then run "vm-model-test (apps/machines/model.js)" node tests/vm_model_test.mjs; fi
    if have node; then run "databases-model-test (apps/databases/model.js)" node tests/databases_model_test.mjs; fi
    if have node; then run "databases-plist-test (apps/databases/plist.js)" node tests/databases_plist_test.mjs; fi
    # where the reading-aloud player goes and how its wave moves (docs/VOICE.md)
    if have node; then run "voiceplayer-model-test (sdk/voiceplayermodel.js)" node tests/voiceplayer_model_test.mjs; fi
    if have node; then run "voicecatalog-test (sdk/voicecatalog.js)" node tests/voicecatalog_test.mjs; fi
    if have node; then run "voicerec-model-test (sdk/voicerecmodel.js)" node tests/voicerec_model_test.mjs; fi
    if have node; then run "vm-catalog-test (apps/machines/catalog.js)" node tests/vm_catalog_test.mjs; fi
    # Machines: crafted images the import must refuse (needs qemu-img and this computer's qemu-system)
    if have qemu-img && { have qemu-system-x86_64 || have qemu-system-aarch64; } && [ -x build/lord-vm ]; then run "vm-import-test" sh tests/vm_import_test.sh build
    else skip "vm-import-test" "qemu-img, qemu-system"; fi
    if have node; then run "screenshots-model-test (apps/screenshots)" node tests/screenshots_model_test.mjs; fi
    if have node; then run "imageedit-model-test (sdk/imageeditmodel.js)" node tests/imageedit_model_test.mjs; fi
    if have node; then run "osk-model-test (sdk/oskmodel.js)" node tests/osk_model_test.mjs
    else skip "osk-model-test (sdk/oskmodel.js)" node; fi
    if have node; then run "motion-model-test (sdk/motionmodel.js)" node tests/motion_model_test.mjs; else skip "motion-model-test" node; fi
    if have node; then run "share-model-test (sdk/sharemodel.js)" node tests/share_model_test.mjs
    else skip "share-model-test" "node"; fi
    if have node; then run "gtk-defaults-test (assets/gtk, #52)" node tests/gtk_defaults_test.mjs; fi
    if have node; then run "dock-model-test (sdk/dockmodel.js)" node tests/dock_model_test.mjs
    else skip "dock-model-test" "node"; fi
    # glibc region names: what the installer and Settings send for every UTF-8 region (sdk/localename.js)
    if have node; then run "localename-test (sdk/localename.js)" node tests/localename_test.mjs
    else skip "localename-test" "node"; fi
    # a region's clock, date order and separators from glibc, and the session's region (sdk/regionformat.js, #45)
    if have node; then run "regionformat-test (sdk/regionformat.js)" node tests/regionformat_test.mjs
    else skip "regionformat-test" "node"; fi
    # Desktops: what shows and who has the keyboard (sdk/desktopmodel.js)
    if have node; then run "desktop-model-test (sdk/desktopmodel.js)" node tests/desktop_model_test.mjs
    else skip "desktop-model-test" "node"; fi
    # session restore's data rules (sdk/sessionmodel.js)
    if have node; then run "session-model-test (sdk/sessionmodel.js)" node tests/session_model_test.mjs
    else skip "session-model-test" "node"; fi
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
    # Settings → Network → VPN's form data (apps/settings/vpnform.js): PEM blocks, edit form, import lines
    if have node; then run "vpnform-test (apps/settings/vpnform.js)" node tests/vpnform_test.mjs
    else skip "vpnform-test" "node"; fi
    # (0.8.7) Monitor → Voltage's model (apps/monitor/voltmodel.js): the rpi-pmic line, states, sources, the trace
    if have node; then run "voltage-model-test (apps/monitor/voltmodel.js)" node tests/voltage_model_test.mjs
    else skip "voltage-model-test" "node"; fi
    # (0.8.8.6) Monitor → GPU's model (apps/monitor/gpumodel.js): engine shares, clients, APIs, AI engines, the Overview
    if have node; then run "gpu-model-test (apps/monitor/gpumodel.js)" node tests/gpu_model_test.mjs
    else skip "gpu-model-test" "node"; fi
    # (0.8.5) System Information's model (apps/sysinfo/model.js): GPU layers, CPU reasons, Compatibility
    if have node; then run "sysinfo-model-test (apps/sysinfo/model.js)" node tests/sysinfo_model_test.mjs
    else skip "sysinfo-model-test" "node"; fi
    # (0.8.8.6) Passwords in the Terminal: the prompt's host and the entries offered (apps/terminal/secret.js)
    if have node; then run "terminal-secret-test (apps/terminal/secret.js)" node tests/terminal_secret_test.mjs
    else skip "terminal-secret-test" "node"; fi
    # (0.8.8.6) files into the Terminal as quoted paths: drops from Files and the clipboard's image (apps/terminal/paths.js)
    if have node; then run "terminal-paths-test (apps/terminal/paths.js)" node tests/terminal_paths_test.mjs
    else skip "terminal-paths-test" "node"; fi
    # (0.8.8.8) the file name under the pointer in ls output, for ❖ + click downloads (apps/terminal/lsnames.js)
    if have node; then run "terminal-lsnames-test (apps/terminal/lsnames.js)" node tests/terminal_lsnames_test.mjs
    else skip "terminal-lsnames-test" "node"; fi
    # which confirmation dialog takes the keys; Enter never presses a destructive button (sdk/dialogkeys.js)
    if have node; then run "dialog-keys-test (sdk/dialogkeys.js)" node tests/dialog_keys_test.mjs
    else skip "dialog-keys-test" "node"; fi
    # (0.8.8.8) a trigger tried on a sample line, for its sheet in Settings (apps/terminal/triggers.js)
    if have node; then run "terminal-triggers-test (apps/terminal/triggers.js)" node tests/terminal_triggers_test.mjs
    else skip "terminal-triggers-test" "node"; fi
    # (0.8.8.8) "Send text at start" runs its last line too; the tags as one field (apps/terminal/profilefields.js)
    if have node; then run "terminal-profilefields-test (apps/terminal/profilefields.js)" node tests/terminal_profilefields_test.mjs
    else skip "terminal-profilefields-test" "node"; fi
    # (0.8.8.6) Monitor → Memory: the processes grouped by app (apps/monitor/memtop.js)
    if have node; then run "monitor-memtop-test (apps/monitor/memtop.js)" node tests/monitor_memtop_test.mjs
    else skip "monitor-memtop-test" "node"; fi
    # (0.8.6) LordBench's model (apps/sysinfo/perf.js): references, verdicts, comparisons
    if have node; then run "sysinfo-perf-test (apps/sysinfo/perf.js)" node tests/sysinfo_perf_test.mjs
    else skip "sysinfo-perf-test" "node"; fi
    # (0.8.7) community hardware reports: the schema, Share on GitHub (apps/sysinfo/share.js), the workflow's
    # checks (tools/hwreport/lib.mjs), the catalogue and its snapshot, the Compatibility panel's community data
    if have node; then run "hwreport-test (tools/hwreport, apps/sysinfo/share.js)" node tests/hwreport_test.mjs
    else skip "hwreport-test" "node"; fi
    # (0.8.7) Store → Updates' names for LordOS's data packages (apps/store/syspkg.js): lordos-hwdata as a dated list
    if have node; then run "store-syspkg-test (apps/store/syspkg.js)" node tests/store_syspkg_test.mjs
    else skip "store-syspkg-test" "node"; fi
    # (0.8.8.4, #37) the Store's search (apps/store/search.js): words in any order, joined or apart, accents, keywords
    if have node; then run "store-search-test (apps/store/search.js)" node tests/store_search_test.mjs
    else skip "store-search-test" "node"; fi
    # (0.8.7) what the live ISO opens by itself, the "Check this computer" boot entry and the menu's height
    if have node; then run "livestart-test (sdk/livestart.js, the ISO's boot menu)" node tests/livestart_test.mjs
    else skip "livestart-test" "node"; fi
    # (0.8.5) Settings → Displays' arrangement (sdk/displaylayout.js): numbers, magnetic snap, valid drops
    if have node; then run "display-layout-test (sdk/displaylayout.js)" node tests/display_layout_test.mjs
    else skip "display-layout-test" "node"; fi
    # (0.8.6) runtime/index.js's skip of unchanged styles sends the same lord.* calls as without it
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "runtime-props-test (runtime/index.js)" node tests/runtime_props_test.mjs
    else skip "runtime-props-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.3, #20) ScrollView's resetKey: a new page starts at the top, the same one keeps its place
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "scrollview-reset-test (sdk/components.jsx)" node tests/scrollview_reset_test.mjs
    else skip "scrollview-reset-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6) ❖⇧4 over a sheet: the selection is in the capture layer, above every other, and a press on it closes nothing
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "capture-layer-test (sdk/components.jsx, sdk/layers.js)" node tests/capture_layer_test.mjs
    else skip "capture-layer-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #48) Lord Classic's power menu: a click on Lock Screen or Switch User reaches the session
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-startmenu-power-test (themes/classic StartMenu)" node tests/classic_startmenu_power_test.mjs
    else skip "classic-startmenu-power-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #58) what the bar shows: Lord Flow's Control Center tiles, the output list, the clock's calendar
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "menubar-test (themes/default ControlCenter, NotificationCenter, MenuBars on every display, sdk/soundoutputs.js)" node tests/menubar_test.mjs
    else skip "menubar-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6) Lord Classic's taskbar on every display: three monitors, panels on the clicked bar's display, one timer
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-taskbar-test (themes/classic on every display, sdk/dock.js drop targets)" node tests/classic_taskbar_test.mjs
    else skip "classic-taskbar-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6) the Terminal's key mappings: iTerm2's keypad, "\n", settings migration 2
    if have node && [ -d ../node_modules/esbuild ]; then
        run "terminal-keymap-test (apps/terminal, sdk/settings.js)" node tests/terminal_keymap_test.mjs
    else skip "terminal-keymap-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.7) the Terminal's own configuration file: the move out of settings.json, a chosen place, read-only
    if have node && [ -d ../node_modules/esbuild ]; then
        run "terminal-config-test (apps/terminal/config.js)" node tests/terminal_config_test.mjs
    else skip "terminal-config-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #55) the keyboard of a user who never chose one: the computer's, settings migration 4
    if have node && [ -d ../node_modules/esbuild ]; then
        run "keyboard-default-test (sdk/keyboard.js, sdk/settings.js)" node tests/keyboard_default_test.mjs
    else skip "keyboard-default-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6) Claude Code in the Terminal's footer: the status line bridge and the footer's model
    run "claude-bridge-test (lord-claude-status)" sh tests/claude_bridge_test.sh
    if have node; then run "terminal-claude-test (apps/terminal/claude.js)" node tests/terminal_claude_test.mjs
    else skip "terminal-claude-test" "node"; fi
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
    # the remote desktop's live encoder (#94): libx264, then VA-API with its first keyframe checked and, with a
    # build whose check always fails, the way out to libx264 (only where a card encodes H.264)
    if [ -x build/lord-media ] && [ -x build/lmstream-test ]; then
        run "lmstream-test (libx264)" build/lmstream-test build/lord-media
        if [ -e /dev/dri/renderD128 ] && [ -x build/lord-media-encfail ]; then
            run "lmstream-test (VA-API)" build/lmstream-test build/lord-media /dev/dri/renderD128
            run "lmstream-test (VA-API fails the check)" build/lmstream-test build/lord-media-encfail /dev/dri/renderD128
        else skip "lmstream-test (VA-API)" "a render node"; fi
    else skip "lmstream-test" "build/lord-media (FFmpeg -dev packages)"; fi
    # document helper: malicious archives and the confinement
    if [ -x build/lord-doc ] && [ -x build/doc-test ] && have python3; then run "doc_evil.sh (lord-doc)" sh tests/doc_evil.sh
    else skip "doc_evil.sh" "build/lord-doc (Poppler and libarchive -dev packages) and python3"; fi
    # text to speech (lord-tts, say): needs a voice model (docs/VOICE.md)
    if [ -x build/lord-tts ] && [ -x build/lord-say ] && [ -x build/doc-test ] && have python3 && { [ -f "$HOME/.local/share/lordos/voice/models/es/standard.gguf" ] || [ -f /usr/share/lordos/voice/models/es/standard.gguf ]; }; then
        run "tts_test.sh (lord-tts, say)" sh tests/tts_test.sh
    else skip "tts_test.sh" "build/lord-tts and a voice model (Settings -> AI -> Voice)"; fi
    # text recognition helper: reading, bad requests and the confinement
    if [ -x build/lord-ocr ] && [ -x build/doc-test ] && [ -r /usr/share/tesseract-ocr/5/tessdata/spa.traineddata ]; then run "ocr_test.sh (lord-ocr)" sh tests/ocr_test.sh
    else skip "ocr_test.sh" "build/lord-ocr (libtesseract-dev) and tesseract-ocr-spa"; fi
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
