#!/bin/sh
# run_suites.sh — runs the test programs that `make test` builds but whose own rules don't run
# them (the other suites run inside their build rule). Called from server/ by the Makefile:
#
#   make -C server test         sh tests/run_suites.sh        self-contained suites, a few seconds
#   make -C server test-full    sh tests/run_suites.sh full   also the helpers (lord-media, lord-doc),
#                                                             the sandbox and the external comparisons
#
# Exits with 1 if any suite fails. A suite whose tool or data isn't installed is reported as
# "skipped: needs X" and doesn't fail the run. Optional data: BIDI_TEST=<BidiCharacterTest.txt>,
# GRAPHEME_TEST=<GraphemeBreakTest.txt>.
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
    # (the Terminal's triggers: each screen line after the absolute row it starts on, a wrapped one whole)
    vt "vt-dump: screen lines with their rows" 'a\r\nerror\r\nabcdefghijkl\r\nerror\r\nb\r\nc' 5 4 5 4 --lines <<'EOF'
--- 5x4 cursor 1,3 history 4 alt 0
h  |a|
h  |error|
h +|abcde|
h +|fghij|
   |kl|
   |error|
   |b|
   |c|
--- 5x4 cursor 1,3 history 4 alt 0
h  |a|
h  |error|
h +|abcde|
h +|fghij|
   |kl|
   |error|
   |b|
   |c|
lines:
2	abcdefghijkl
5	error
6	b
7	c
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
    # grapheme clusters (what the arrows and Backspace step over) and the caret's edges in mixed directions
    gbt=${GRAPHEME_TEST:-}
    for f in /usr/share/unicode/auxiliary/GraphemeBreakTest.txt /usr/share/unicode-data/auxiliary/GraphemeBreakTest.txt tests/GraphemeBreakTest.txt; do
        [ -z "$gbt" ] && [ -r "$f" ] && gbt=$f
    done
    if [ -n "$gbt" ] && [ -r "$gbt" ]; then run "bidi-test: carets and $(basename "$gbt")" build/bidi-test "$gbt"
    else run "bidi-test: carets" build/bidi-test; skip "bidi-test: graphemes" "GraphemeBreakTest.txt (unicode-data, or GRAPHEME_TEST=<file>)"; fi

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
    run "net-test wifi-saved" build/net-test wifi-saved
    run "pkg-test" build/pkg-test
    run "fsops-test" build/fsops-test
    run "portal-check-test" build/portal-check-test
    if have dbus-run-session; then run "portal-gs-test (GlobalShortcuts over a private bus)" dbus-run-session -- build/portal-gs-test
    else skip "portal-gs-test" "dbus-run-session"; fi
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
    if have node; then run "logfile-test (sdk/logfile.js)" node tests/logfile_test.mjs; fi
    if have node; then run "menubar-model-test (sdk/menubarmodel.js, sdk/ticker.js)" node tests/menubar_model_test.mjs; fi
    if have node; then run "editor-scrollbar-test (apps/editor/scrollbar.js)" node tests/editor_scrollbar_test.mjs; fi
    if have node; then run "editor-recent-test (apps/editor/recent.js, File → Open Recent, #113)" node tests/editor_recent_test.mjs; fi
    if have node; then run "mem-figure-test (sdk/memfigure.js)" node tests/mem_figure_test.mjs; fi
    if have node; then run "photos-places-test (apps/photos/places.js)" node tests/photos_places_test.mjs; fi
    if have node; then run "photos-trash-test (apps/photos/trash.js)" node tests/photos_trash_test.mjs; fi
    if have node; then run "bt-power-model-test (sdk/btpowermodel.js)" node tests/bt_power_model_test.mjs; fi
    if have node; then run "archive-vol-test (sdk/archivevol.js)" node tests/archive_vol_test.mjs; fi
    if have node; then run "bt-audio-model-test (sdk/btaudiomodel.js)" node tests/bt_audio_model_test.mjs; fi
    if have node; then run "bt-list-model-test (sdk/btlistmodel.js)" node tests/bt_list_model_test.mjs; fi
    if have node; then run "kbinhibit-model-test (sdk/kbinhibitmodel.js)" node tests/kbinhibit_model_test.mjs; fi
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
    # a field's caret by grapheme cluster and on screen in mixed directions (sdk/textfieldmodel.js, lord.text.carets)
    if have node; then run "textfield-cluster-test (sdk/textfieldmodel.js)" node tests/textfield_cluster_test.mjs
    else skip "textfield-cluster-test" "node"; fi
    # a password field's eye and its Caps Lock hint, shown and hidden (sdk/passwordfieldmodel.js)
    if have node; then run "password-field-test (sdk/passwordfieldmodel.js)" node tests/password_field_test.mjs
    else skip "password-field-test" "node"; fi
    if have node; then run "textarea-model-test (sdk/textareamodel.js)" node tests/textarea_model_test.mjs; fi
    if have node; then run "filekinds-test (sdk/filekinds.js)" node tests/filekinds_test.mjs; fi
    if have node; then run "thumb-cache-test (sdk/thumbcache.js)" node tests/thumb_cache_test.mjs; fi
    # Cloud without a screen: status glyphs, space, providers, menus, sidebar, menu bar, notifications, the mock's shape (sdk/cloudmodel.js)
    if have node; then run "cloud-model-test (sdk/cloudmodel.js)" node tests/cloud_model_test.mjs
    else skip "cloud-model-test" "node"; fi
    # a browser sign-in against a stand-in rclone: a second account, a drive that can't be opened, none (#87)
    if have python3 && [ -x build/cloud-harness ]; then run "cloud-signin-fake (src/cloud, OneDrive's drive choice, turning accounts off)" sh tests/cloud_signin_fake.sh build
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
    # (0.8.8.8, #112) Lord Flow's minimize effect: which windows start it, none with Reduce motion
    if have node; then run "minimize-model-test (themes/default/minimize.js)" node tests/minimize_model_test.mjs; else skip "minimize-model-test" node; fi
    if have node; then run "share-model-test (sdk/sharemodel.js)" node tests/share_model_test.mjs
    else skip "share-model-test" "node"; fi
    if have node; then run "gtk-defaults-test (assets/gtk, #52)" node tests/gtk_defaults_test.mjs; fi
    if have node; then run "dock-model-test (sdk/dockmodel.js)" node tests/dock_model_test.mjs
    else skip "dock-model-test" "node"; fi
    # (0.8.8.8) deactivated LordOS apps: the core list, the setting, launcher/search, default apps, Open With, VMs, mail pauses
    if have node; then run "apps-off-model-test (sdk/appsoffmodel.js)" node tests/apps_off_model_test.mjs
    else skip "apps-off-model-test" "node"; fi
    # (0.8.8.8) web apps: the window's app id and its process, the Dock's profiles menu, the Store's address sheet,
    # LordOS's catalog (assets/webapps), Settings' rows (sdk/webappsmodel.js, docs/WEBAPPS.md)
    if have node; then run "webapps-model-test (sdk/webappsmodel.js)" node tests/webapps_model_test.mjs
    else skip "webapps-model-test" "node"; fi
    # (0.8.8.8) web apps follow the desktop's light or dark mode: the matchMedia script lord-web adds (src/web/webscheme.js)
    if have node; then run "webscheme-test (src/web/webscheme.js)" node tests/webscheme_test.mjs
    else skip "webscheme-test" "node"; fi
    # (0.8.8.8) Settings → Notifications → Sound: Default, None or one of LordOS's sounds (sdk/notifytones.js)
    if have node; then run "notify-tones-test (sdk/notifytones.js)" node tests/notify_tones_test.mjs
    else skip "notify-tones-test" "node"; fi
    # glibc region names: what the installer and Settings send for every UTF-8 region (sdk/localename.js)
    if have node; then run "localename-test (sdk/localename.js)" node tests/localename_test.mjs
    else skip "localename-test" "node"; fi
    # a region's clock, date order and separators from glibc, and the session's region (sdk/regionformat.js, #45)
    if have node; then run "regionformat-test (sdk/regionformat.js)" node tests/regionformat_test.mjs
    else skip "regionformat-test" "node"; fi
    # Desktops: what shows and who has the keyboard (sdk/desktopmodel.js)
    if have node; then run "desktop-model-test (sdk/desktopmodel.js)" node tests/desktop_model_test.mjs
    else skip "desktop-model-test" "node"; fi
    # (0.8.8.8) a Linux app's dialog kept with its window: over it, moved with it, a modal one above it (sdk/dialogmodel.js)
    if have node; then run "dialog-model-test (sdk/dialogmodel.js)" node tests/dialog_model_test.mjs
    else skip "dialog-model-test" "node"; fi
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
    if have node; then run "utf8-test (sdk/utf8.js; no TextEncoder/TextDecoder in desktop code)" node tests/utf8_test.mjs
    else skip "utf8-test" "node"; fi
    # (0.8.8.8) the primary selection of the text fields: what a selection publishes, where a middle click pastes (sdk/primarymodel.js)
    if have node; then run "primary-model-test (sdk/primarymodel.js)" node tests/primary_model_test.mjs
    else skip "primary-model-test" "node"; fi
    # (0.8.8.8) Now Playing and the media keys with the Linux players (MPRIS): which one the keys reach (sdk/mprismodel.js)
    if have node; then run "mpris-model-test (sdk/mprismodel.js)" node tests/mpris_model_test.mjs
    else skip "mpris-model-test" "node"; fi
    # (0.8.8.8) a forward whose fixed local port is taken: told once, moved to a free port (apps/terminal/fwdports.js)
    if have node; then run "terminal-fwdports-test (apps/terminal/fwdports.js)" node tests/terminal_fwdports_test.mjs
    else skip "terminal-fwdports-test" "node"; fi
    # what a paste in Files copies: its own cut or copy, or the files a Linux app copied (apps/files/clipfiles.js)
    if have node; then run "files-clipfiles-test (apps/files/clipfiles.js)" node tests/files_clipfiles_test.mjs
    else skip "files-clipfiles-test" "node"; fi
    # a paste of many items: the copies past lord-server's job slots wait their turn (apps/files/jobqueue.js, GitHub #104)
    if have node; then run "files-jobqueue-test (apps/files/jobqueue.js)" node tests/files_jobqueue_test.mjs
    else skip "files-jobqueue-test" "node"; fi
    # the column view: what is picked in a deeper column is selected (apps/files/colview.js, GitHub #100)
    if have node; then run "files-colview-test (apps/files/colview.js)" node tests/files_colview_test.mjs
    else skip "files-colview-test" "node"; fi
    # menus from the keyboard: arrows, type-ahead, the submenu's side and the safe triangle (sdk/menunav.js)
    if have node; then run "menu-nav-test (sdk/menunav.js)" node tests/menu_nav_test.mjs
    else skip "menu-nav-test" "node"; fi
    # which confirmation dialog takes the keys; Enter never presses a destructive button (sdk/dialogkeys.js)
    if have node; then run "dialog-keys-test (sdk/dialogkeys.js)" node tests/dialog_keys_test.mjs
    else skip "dialog-keys-test" "node"; fi
    # (0.8.8.8) keyboard focus: the Tab order over the tree, skipping, focus-visible, the keys of sliders, menus and scroll views (sdk/focusmodel.js)
    if have node; then run "focus-model-test (sdk/focusmodel.js)" node tests/focus_model_test.mjs
    else skip "focus-model-test" "node"; fi
    # (0.8.8.8) List and Table selection: Shift ranges, Ctrl toggles, the page keys, Ctrl+A and type-to-find (sdk/listmodel.js)
    if have node; then run "list-model-test (sdk/listmodel.js)" node tests/list_model_test.mjs
    else skip "list-model-test" "node"; fi
    # (0.8.8.8) the keys of Tabs, radio groups and a Dropdown, the wheel on sliders, tooltips and progress (sdk/controlmodel.js)
    if have node; then run "control-model-test (sdk/controlmodel.js)" node tests/control_model_test.mjs
    else skip "control-model-test" "node"; fi
    # (0.8.8.8) files dragged from Files onto Notes, Photos, Player and PDF Viewer: which ones each takes (sdk/filedrop.js)
    if have node; then run "filedrop-test (sdk/filedrop.js)" node tests/filedrop_test.mjs
    else skip "filedrop-test" "node"; fi
    # (0.8.8.8) a Linux app's OpenURI request: web addresses, files, and asking when the app is not in front (sdk/portalopenmodel.js)
    if have node; then run "portal-open-model-test (sdk/portalopenmodel.js)" node tests/portal_open_model_test.mjs
    else skip "portal-open-model-test" "node"; fi
    # (0.8.8.8) a Linux app's global shortcuts: the sheet's rows, the combination a key names, what is refused (sdk/globalshortcutsmodel.js)
    if have node; then run "global-shortcuts-model-test (sdk/globalshortcutsmodel.js)" node tests/global_shortcuts_model_test.mjs
    else skip "global-shortcuts-model-test" "node"; fi
    # (0.8.8.8) a Linux app's ScreenCast chooser: what is offered, one or several, what lord-server gets (sdk/screencastmodel.js)
    if have node; then run "screencast-model-test (sdk/screencastmodel.js)" node tests/screencast_model_test.mjs
    else skip "screencast-model-test" "node"; fi
    # (0.8.8.8) Linux apps' tray icons: which show, a dbusmenu layout as the SDK's menu, where it opens (sdk/traymodel.js)
    if have node; then run "tray-model-test (sdk/traymodel.js)" node tests/tray_model_test.mjs
    else skip "tray-model-test" "node"; fi
    # (0.8.8.8) SVG icons of Linux apps: PNG as it is, SVG at the size shown, symbolic ones in the bar's color (sdk/iconsrc.js)
    if have node; then run "iconsrc-test (sdk/iconsrc.js)" node tests/iconsrc_test.mjs
    else skip "iconsrc-test" "node"; fi
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
    # (0.8.8.8) only the theme's ThemeProvider tells Linux apps (the portal) and isolated apps the desktop's colors, so a
    # nested one (Notes' light paper) can't flip the color scheme back and forth (Chrome's profile picker flickered)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "theme-portal-scene (sdk/theme.js ThemeProvider)" node tests/textfield_menu_test.mjs theme_portal_scene.mjs
    else skip "theme-portal-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) text fields: a right click keeps the caret and opens Undo…Select All, Ctrl+Z/Ctrl+Shift+Z, a triple click
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "textfield-menu-test (sdk/components.jsx TextInput, TextArea)" node tests/textfield_menu_test.mjs
    else skip "textfield-menu-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8, #117) Notes: Edit and Format menus and the editor's right-click menu send the editor's own keys
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "notes-editor-scene (apps/notes/NoteEditor.jsx, editmenu.js)" node tests/textfield_menu_test.mjs notes_editor_scene.mjs
    else skip "notes-editor-scene" "node and the repository's node_modules (npm install)"; fi
    # a password TextInput: the eye shows it and keeps the focus, copies nothing, hides on blur; ⇪ follows Caps Lock
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "password-field-scene (sdk/components.jsx TextInput secure)" node tests/textfield_menu_test.mjs password_field_scene.mjs
    else skip "password-field-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) the ScreenCast chooser: the app's name, the pictures, Share only with a pick, Cancel, the app closing it (sdk/screencast.jsx)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "screencast-scene (sdk/screencast.jsx)" node tests/textfield_menu_test.mjs screencast_scene.mjs
    else skip "screencast-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) the Store's "Add from an address" sheet: https only, the origin lord-web read, Install sends no URL
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "webapps-sheet-scene (apps/store/addweb.js)" node tests/textfield_menu_test.mjs webapps_sheet_scene.mjs
    else skip "webapps-sheet-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) Get of a web app without lordos-web: the engine's plan and sizes, the package job, then the app with no click
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "webapps-get-scene (apps/store/getweb.js)" node tests/textfield_menu_test.mjs webapps_get_scene.mjs
    else skip "webapps-get-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) Tab between the SDK's controls, the focus ring, Space and Enter, the arrows, a press that ends the focus (sdk/focus.js)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "focus-scene-test (sdk/focus.js, sdk/components.jsx, sdk/controls.jsx)" node tests/focus_scene_test.mjs
    else skip "focus-scene-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) the List, the Table, Tabs, Dropdown, RadioGroup, a Popover's focus and the wheel on a Slider, from the keys
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "list-scene (sdk/lists.jsx, sdk/controls.jsx)" node tests/focus_scene_test.mjs list_scene.mjs
    else skip "list-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) menus from the keyboard in both themes: arrows, type-ahead, submenus, the hover pause, F10 and ⌥ alone
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "menu-keys-test (themes/default Menu, themes/classic Menu, sdk/shortcuts.js)" node tests/menu_keys_test.mjs
    else skip "menu-keys-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #48) Lord Classic's power menu: a click on Lock Screen or Switch User reaches the session
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-startmenu-power-test (themes/classic StartMenu)" node tests/classic_startmenu_power_test.mjs
    else skip "classic-startmenu-power-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #58) what the bar shows: Lord Flow's Control Center tiles, the output list, the clock's calendar
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "menubar-test (themes/default ControlCenter, NotificationCenter, MenuBars on every display, sdk/soundoutputs.js)" node tests/menubar_test.mjs
    else skip "menubar-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8, #99) Files' column view: a tagged item shows its tag dots after the name, as the list view does
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "files-colview-scene (apps/files/views.jsx ColumnView)" node tests/menubar_test.mjs files_colview_scene.mjs
    else skip "files-colview-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6) Lord Classic's taskbar on every display: three monitors, panels on the clicked bar's display, one timer
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-taskbar-test (themes/classic on every display, sdk/dock.js drop targets)" node tests/classic_taskbar_test.mjs
    else skip "classic-taskbar-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8, #111) how many app buttons fit on a Classic taskbar, and how big
    if have node; then run "classic-taskbarfit-test (themes/classic/taskbarfit.js)" node tests/classic_taskbarfit_test.mjs; fi
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
    # (0.8.8.8) web apps (docs/WEBAPPS.md): lord-web's probe on a page served by the test itself, headless, with
    # WebKit's sandbox; the answer, the icon step and a throwaway profile that leaves nothing
    if [ -x build/lord-web ] && have python3 && have bwrap && have xdg-dbus-proxy; then
        run "web-probe-test (lord-web --probe)" python3 -I tests/web_probe_test.py build/lord-web
    else skip "web-probe-test" "wpe-webkit-2.0 (build/lord-web), bubblewrap, xdg-dbus-proxy and python3"; fi
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
    # (0.8.8.8) SVG icons through lord-media (MW_SVG, helper/lm_svg.c): a valid icon, a symbolic one recolored, then
    # malformed markup, huge numbers, deep nesting (also behind a comment) and a file over 1 MB, which must be
    # refused, not crash it
    if [ -x build/lord-media ] && have python3; then
        python3 -I - "$T" <<'EOF'
import os, sys
d = sys.argv[1]
def put(name, text):
    with open(os.path.join(d, name), 'w') as f: f.write(text)
icon = ('<svg width="16" height="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg">'
        '<path d="m 7 1 l -3.2 3.6 h -1 c -1.1 0 -2 0.8 -2 2 v 2 c 0 1.1 0.9 2 2 2 h 1 l 3.2 3.6 z" fill="#2e3436"/>'
        '<rect x="10" y="3" width="2" height="10" fill="#2e3436"/></svg>')
put('icon.svg', icon)
put('bad.svg', '<svg width="16" height="16"><path d="M 0 0 L 16')
put('huge.svg', '<svg width="16" height="16"><path d="M 1e38 1e38 L -1e38 0 L 0 1e999 Z"/></svg>')
put('deep.svg', '<svg width="16" height="16">' + '<g>' * 100000 + '<rect width="16" height="16"/></svg>')
# the same nesting where an XML reader sees a comment: nanosvg ends the tag at the first '>' and parses it all
put('hidden.svg', '<svg width="16" height="16"><!-- > ' + '<g>' * 100000 + ' --><rect width="16" height="16"/></svg>')
put('big.svg', '<svg width="16" height="16"><!--' + 'x' * (1 << 20) + '--><rect width="16" height="16"/></svg>')
EOF
        run "media-test svg (an icon)" build/media-test svg "$T/icon.svg" 48 - ok
        run "media-test svg (symbolic, recolored)" build/media-test svg "$T/icon.svg" 24 ff8000 ok
        run "media-test svg (malformed)" build/media-test svg "$T/bad.svg" 32 - fail
        run "media-test svg (huge numbers)" build/media-test svg "$T/huge.svg" 32 - fail
        run "media-test svg (deep nesting)" build/media-test svg "$T/deep.svg" 32 - fail
        run "media-test svg (nesting behind a comment)" build/media-test svg "$T/hidden.svg" 32 - fail
        run "media-test svg (over 1 MB)" build/media-test svg "$T/big.svg" 32 - fail
    else skip "media-test svg" "build/lord-media (FFmpeg -dev packages) and python3"; fi
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
