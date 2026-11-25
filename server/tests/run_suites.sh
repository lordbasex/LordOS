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
    # (#163) Shift alone (+, sent before every capital) keeps the syllable: ㅆ and ㄲ as finals, ㅒ and ㅖ after an initial
    ime "ime-test: hangul Shift jamo" hangul 'dl+T_qk+R_rk+Rk_dl+Tdj_r+O_d+P_+dl+T_' "" <<'EOF'
committed: 있 밖 가까 있어 걔 예 있
EOF
    # every double final consonant, and how one splits when a vowel follows
    ime "ime-test: hangul double finals" hangul 'rkrt_ahrt_dksw_dksg_ekfr_tkfa_sjfq_rhft_gkfx_dmfv_dlfg_djqt_ekfrl_dkswdk_djqtdj_' "" <<'EOF'
committed: 갃 몫 앉 않 닭 삶 넓 곬 핥 읊 잃 없 달기 앉아 없어
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

    # the toolkit audit's plain X11 harness (lordos-testbench --toolkits, docs/TOOLKIT-AUDIT.md) against a private Xvfb
    if ! have python3; then skip "toolkit-audit x11.py" "python3"
    elif ! have Xvfb; then skip "toolkit-audit x11.py" "Xvfb"
    else run "toolkit-audit x11.py" python3 -I ../tools/toolkit-audit/x11_test.py; fi

    run "sound-test" build/sound-test
    # lordos-issue's parsers of untrusted text (packaging/bin/lordos-issue): the WAYLAND_DEBUG scrubbing, the programs on the
    # Wayland and X11 sockets, lordos-testbench --globals' summary, --app with a fake program (tests/data/lordos-issue)
    run "lordos-issue-test (WAYLAND_DEBUG scrubbing, connected programs, --app)" sh tests/lordos_issue_test.sh
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
    run "net-test wireless" build/net-test wireless
    run "net-test wifi-status" build/net-test wifi-status
    run "pkg-test" build/pkg-test
    # (0.8.8.9) lordos + lordos-web updated together: the engine's dpkg order against a real dpkg in a throwaway root
    if have dpkg && have dpkg-deb; then run "pkg-breaks-dpkg-test (lordos + lordos-web, dpkg --root)" sh tests/pkg_breaks_dpkg_test.sh
    else skip "pkg-breaks-dpkg-test" "dpkg and dpkg-deb"; fi
    run "fsops-test" build/fsops-test
    run "portal-check-test" build/portal-check-test
    if have dbus-run-session; then run "portal-gs-test (GlobalShortcuts over a private bus)" dbus-run-session -- build/portal-gs-test
    else skip "portal-gs-test" "dbus-run-session"; fi
    if have dbus-run-session; then run "portal-cast-test (ScreenCast restore token, Screenshot, FileChooser lending)" dbus-run-session -- build/portal-cast-test
    else skip "portal-cast-test" "dbus-run-session"; fi
    run "fsasync-test" build/fsasync-test
    run "bgsave-test (#250: saves off the interface's thread)" build/bgsave-test
    run "sign-test" build/sign-test
    # migration 008: a tablet's power-button defaults (packaging/migrations, issue #131)
    run "migration 008 tablet-power" sh tests/migration_tablet_power_test.sh
    # (#187) another display manager enabled next to lordos.service: the package's postinst warns (packaging/bin)
    run "dm-check-test (packaging/bin/lordos-dm-check)" sh tests/dm_check_test.sh
    # (0.8.8.9) the Chromium-based browsers' initial preferences (src/pkg/browserprefs.c) and migration 009
    run "browserprefs-test" build/browserprefs-test
    run "migration 009 chromium-family" sh tests/migration_chromium_family_test.sh
    # (0.8.9.6) shared memory's huge pages for Machines: shmem_enabled never -> advise, nothing else (docs/VIRTUALIZATION.md 3.7.2)
    run "migration 011 shmem-huge-pages" sh tests/migration_shmem_huge_test.sh
    # (0.8.9.6) LordOS as a Machines guest: the agent port for the video group, lordos-guest started after the update
    run "migrations 012, 013 guest-agent port" sh tests/migration_guest_port_test.sh
    # the LordOS Guest Tools disc's installer: its choices per package manager, init system and prompt (tools/guest/install.sh)
    run "guest-tools install.sh (--plan)" sh tests/guest_tools_install_test.sh
    # fastfetch / neofetch fall back to lordos-fetch; the real package wins (packaging/fallback-bin)
    run "fetch-fallback (fastfetch, neofetch)" sh tests/fetch_fallback_test.sh
    run "mmdb-test" build/mmdb-test
    run "canvas-test" build/canvas-test
    run "imgops-test" build/imgops-test
    run "arcpath-test" build/arcpath-test
    run "ipp-test" build/ipp-test
    run "tp-test" build/tp-test
    run "fan-test" build/fan-test
    run "term-damage-test" build/term-damage-test
    run "freespace-test" build/freespace-test
    run "install-plan-test" build/install-plan-test
    # the firewall rule sheet's parser (sdk/fwrule.js) against the same cases as build/fwdest-test
    if have node; then run "remotecert-test (sdk/remotecert.js)" node tests/remotecert_test.mjs; fi
    if have node; then run "remotedesk-test (sdk/remotedeskmodel.js)" node tests/remotedesk_test.mjs; fi
    if have node; then run "place-model-test (sdk/placemodel.js)" node tests/place_model_test.mjs; fi
    if have node; then run "protect-model-test (sdk/protectmodel.js, Machines' protected machines)" node tests/protect_model_test.mjs; fi
    run "spectrum-test" build/spectrum-test
    if have node; then run "soundwave-model-test (sdk/soundwavemodel.js, widgetgrid.js pins)" node tests/soundwave_model_test.mjs; fi
    if have node; then run "crashreport-test (sdk/crashreport.js, an app crash's Copy details)" node tests/crashreport_test.mjs; fi
    if have node; then run "statusplace-test (sdk/statusplace.js)" node tests/statusplace_test.mjs; fi
    if have node; then run "logfile-test (sdk/logfile.js)" node tests/logfile_test.mjs; fi
    if have node; then run "browserframe-notes-test (sdk/browserframe.js, #222)" node tests/browserframe_notes_test.mjs; fi
    if have node; then run "menubar-model-test (sdk/menubarmodel.js, sdk/ticker.js)" node tests/menubar_model_test.mjs; fi
    if have node; then run "editor-scrollbar-test (apps/editor/scrollbar.js)" node tests/editor_scrollbar_test.mjs; fi
    if have node; then run "editor-recent-test (apps/editor/recent.js, File → Open Recent, #113)" node tests/editor_recent_test.mjs; fi
    if have node; then run "backup-model-test (apps/backup/model.js exclusions, FilePicker cloud={false}, #200)" node tests/backup_model_test.mjs; fi
    if have node; then run "mem-figure-test (sdk/memfigure.js)" node tests/mem_figure_test.mjs; fi
    if have node; then run "openwith-model-test (sdk/openwithmodel.js, Files' Open with providers)" node tests/openwith_model_test.mjs; fi
    if have node; then run "vm-openwith-test (apps/machines/openwith.js, a machine in Open with)" node tests/vm_openwith_test.mjs; fi
    if have node; then run "vm-xfer-test (apps/machines/xfer.js, apps/files/quarantine.js: the file copy panel, files from a machine)" node tests/vm_xfer_test.mjs; fi
    if have node; then run "guest-apps-test (sdk/guestappsmodel.js, guest apps in the launcher)" node tests/guest_apps_test.mjs; fi
    if have node; then run "theme-gallery-test (sdk/themegallery.js, Settings → Appearance)" node tests/theme_gallery_test.mjs; fi
    if have node; then run "gpu-report-test (sdk/gpureport.js, Settings → Graphics cards' compatibility report)" node tests/gpu_report_test.mjs; fi
    if have node; then run "iso-firmware-test (tools/iso/build.sh firmware list vs the Store map, #227)" node tests/iso_firmware_test.mjs; fi
    if have node; then run "vm-cli-test (apps/machines/model.js cliPlan, the machines command's session requests)" node tests/vm_cli_test.mjs; fi
    if have node; then run "vm-group-model-test (apps/machines/model.js groupPlan: a machine group's start and stop order, its link)" node tests/vm_group_model_test.mjs; fi
    if have node; then run "vm-storage-model-test (apps/machines/storagemodel.js: key steps, recovery key, disposable, Export)" node tests/vm_storage_model_test.mjs; fi
    if have node; then run "vm-usb-model-test (apps/machines/model.js: host USB devices' menu rows, rules, indicator, permissions)" node tests/vm_usb_model_test.mjs; fi
    if have node; then run "vm-group-ui-test (apps/machines/group.js and catalog.js: a group's one card, its menu, Start Fresh, its budget, the Whonix card)" node tests/vm_group_ui_test.mjs; fi
    if have node; then run "vm-profiles-test (apps/machines/profiles.js, systems.js: .lordprofile import, your systems, PNG logos, library chips)" node tests/vm_profiles_test.mjs; fi
    if have node; then run "photos-places-test (apps/photos/places.js)" node tests/photos_places_test.mjs; fi
    if have node; then run "photos-trash-test (apps/photos/trash.js)" node tests/photos_trash_test.mjs; fi
    if have node; then run "bt-power-model-test (sdk/btpowermodel.js)" node tests/bt_power_model_test.mjs; fi
    if have node; then run "archive-vol-test (sdk/archivevol.js)" node tests/archive_vol_test.mjs; fi
    if have node; then run "wayland-table-test (docs/wayland-protocols.json against server/src/wl's globals, generated table up to date)" node tests/wayland_table_test.mjs; else skip "wayland-table-test" "node"; fi
    if have node; then run "wayland-import-test (tools/wayland-app-import.mjs reads wayland.app's bundle as literals, never runs it)" node tests/wayland_import_test.mjs; else skip "wayland-import-test" "node"; fi
    if have node; then run "bt-audio-model-test (sdk/btaudiomodel.js)" node tests/bt_audio_model_test.mjs; fi
    if have node; then run "bt-list-model-test (sdk/btlistmodel.js)" node tests/bt_list_model_test.mjs; fi
    if have node; then run "kbinhibit-model-test (sdk/kbinhibitmodel.js)" node tests/kbinhibit_model_test.mjs; fi
    if have node; then run "windowbuttons-test (sdk/windowbuttons.js)" node tests/windowbuttons_test.mjs; fi
    if have node; then run "moddrag-model-test (sdk/moddragmodel.js, sdk/windowbuttons.js size and tiling, #151, #219)" node tests/moddrag_model_test.mjs; fi
    if have node; then run "celeste-model-test (themes/celeste/model.js)" node tests/celeste_model_test.mjs; fi
    if have node; then run "login-model-test (sdk/loginmodel.js)" node tests/login_model_test.mjs; fi
    if have node; then run "diskplaces-test (sdk/diskplaces.js, #122)" node tests/diskplaces_test.mjs; fi
    if have node; then run "jsx-undeclared-test (every JSX component is declared, #174)" node tests/jsx_undeclared_test.mjs; fi
    if have node; then run "greeter-inbox-test (themes/greeter/inbox.js, #172)" node tests/greeter_inbox_test.mjs; fi
    # Settings → Flatpak with 200 000 files to measure: never on the main thread (the 2 s freeze of 2026-11-23)
    sh tests/settings_nofreeze.sh >"$T/out" 2>&1
    case $? in
        0) ok "settings-nofreeze (tests/settings_nofreeze.sh, headless lord-server: Settings → Flatpak)" ;;
        77) skip "settings-nofreeze" "build/lord-server, node and the default theme (make, npm run build)" ;;
        *) bad "settings-nofreeze (tests/settings_nofreeze.sh, headless lord-server: Settings → Flatpak)" ;;
    esac
    # Files → Get Info on a folder of 100 000 files: its size never on the main thread (2026-11-23)
    sh tests/files_info_nofreeze.sh >"$T/out" 2>&1
    case $? in
        0) ok "files-info-nofreeze (tests/files_info_nofreeze.sh, headless lord-server: Files → Get Info)" ;;
        77) skip "files-info-nofreeze" "build/lord-server, node and the default theme (make, npm run build)" ;;
        *) bad "files-info-nofreeze (tests/files_info_nofreeze.sh, headless lord-server: Files → Get Info)" ;;
    esac
    # a popup opened again on the same wl_surface, as GTK 4's popovers, against a headless lord-server (#159)
    sh tests/popup_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "popup-reopen (tests/popup_client.c, headless lord-server, #159)" ;;
        77) skip "popup-reopen" "build/lord-server and the default theme (make, npm run build)" ;;
        *) bad "popup-reopen (tests/popup_client.c, headless lord-server, #159)" ;;
    esac
    # (#219) Move and resize with: Off (the default) takes no press; ❖ + drag moves and resizes a Wayland and an X11 window
    sh tests/moddrag_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "moddrag (tests/moddrag_client.c, moddrag_x11.c, headless lord-server-test: off, move, resize, lock, shortcuts)" ;;
        77) skip "moddrag" "build/lord-server-test and the default theme (make, npm run build)" ;;
        *) bad "moddrag (tests/moddrag_client.c, moddrag_x11.c, headless lord-server-test)"; cat "$T/out" ;;
    esac
    # (#232) a fullscreen window's desync subsurface (Firefox's page) repaints its own box, checked against a full repaint
    sh tests/fsub_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "fsub (tests/fsub_client.c, headless lord-server-test: a fullscreen window's subsurface repaints its box, #232)" ;;
        77) skip "fsub" "build/lord-server-test and the default theme (make, npm run build)" ;;
        *) bad "fsub (tests/fsub_client.c, headless lord-server-test, #232)"; cat "$T/out" ;;
    esac
    # (#209) an X11 app's menus: an override-redirect and a POPUP_MENU window shown where X put them, clicked, hidden
    sh tests/xpopup_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "xpopup (tests/xpopup_x11.c, headless lord-server-test + Xwayland: X11 menus shown, clicked, hidden, #209)" ;;
        77) skip "xpopup" "build/lord-server-test, build/xpopup-x11 (libxcb1-dev), Xwayland and the default theme (make, npm run build)" ;;
        *) bad "xpopup (tests/xpopup_x11.c, headless lord-server-test + Xwayland, #209)"; cat "$T/out" ;;
    esac
    # a Whonix group's members (docs/VIRTUALIZATION.md 19.5): each one's QEMU and passt arguments against Whonix's own
    # libvirt XML of the pinned release; started paused from two empty disks, nothing boots
    bash tests/vm_whonix_xml_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-whonix-xml (tests/vm_whonix_xml_test.sh: the members' QEMU arguments against Whonix's XML)" ;;
        77) skip "vm-whonix-xml" "KVM, QEMU, qemu-img, passt, unshare and python3" ;;
        *) bad "vm-whonix-xml (tests/vm_whonix_xml_test.sh)"; cat "$T/out" ;;
    esac
    # Machines' settings → QEMU arguments as a table (docs/VIRTUALIZATION.md 12): a throwaway machine per row, started
    # paused (nothing boots), its logged command line checked for what each setting adds and must not
    python3 -I tests/vm_qemu_args_test.py >"$T/out" 2>&1
    case $? in
        0) ok "vm-qemu-args (tests/vm_qemu_args_test.py: $(grep -c '^ok:' "$T/out") rows of settings against QEMU's arguments)" ;;
        77) skip "vm-qemu-args" "KVM, QEMU, qemu-img, passt, unshare and python3" ;;
        *) bad "vm-qemu-args (tests/vm_qemu_args_test.py)"; cat "$T/out" ;;
    esac
    # protected windows (docs/PRIVACY.md): drawn on the display, the protected fill in a screenshot, the remote desktop's
    # and recording's frame, the Screenshot portal's still and a window shared alone; the other window captured as shown
    sh tests/protected_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "protected-windows (tests/protected_headless.sh, headless lord-server-test: output, screenshot, mirror, portal, window share)" ;;
        77) skip "protected-windows" "build/lord-server-test, python3 and node with esbuild (make, npm install)" ;;
        *) bad "protected-windows (tests/protected_headless.sh, headless lord-server-test)"; cat "$T/out" ;;
    esac
    # lordos-testbench --globals: every global of LordOS's protocol table offered at its version (an ordinary client with
    # the fake headset plugged in, and the Xwayland-like connection for xwayland_shell_v1)
    sh tests/testbench_globals_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "testbench-globals (tests/testbench_globals_headless.sh, headless lord-server-test: $(sed -n 's/^together: //p' "$T/out"))" ;;
        77) skip "testbench-globals" "build/lord-server-test, build/lordos-testbench, the default theme and python3" ;;
        *) bad "testbench-globals (tests/testbench_globals_headless.sh, headless lord-server-test)" ;;
    esac
    # Lord Workspaces: a window launched from a display's bar lands on that display, in that activity and its slot, even
    # after a profile picker and with another activity in view; a dialog told its parent late goes over it; the bar
    # marks a minimized window (themes/workspaces/model/launches.js; left to right and right to left)
    for lp in en ar; do
        mkdir -p -m 700 "$T/launchplace"; LAUNCHPLACE_LANG=$lp LAUNCHPLACE_DIR="$T/launchplace" sh tests/launchplace_headless.sh >"$T/out" 2>&1
        case $? in
            0) ok "launch-placement-$lp (tests/launchplace_headless.sh, headless lord-server-test, a fake Chrome with a profile picker)" ;;
            77) skip "launch-placement-$lp" "build/lord-server-test, build/launchplace-client, python3 and node with esbuild" ;;
            *) bad "launch-placement-$lp (tests/launchplace_headless.sh)"; cat "$T/out" ;;
        esac
    done
    # launching an app that already runs (a browser's single instance) shows its window, even on a desktop not in view,
    # and a copy running with no window shows a new one (the 2026-11-23 Google Chrome case)
    sh tests/handoff_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "handoff (tests/handoff_client.c, headless lord-server: relaunch of a running app, Lord Flow and Lord Workspaces)" ;;
        77) skip "handoff" "build/lord-server, build/handoff-client and the default theme (make, npm run build)" ;;
        *) bad "handoff (tests/handoff_client.c, headless lord-server: relaunch of a running app)"; cat "$T/out" ;;
    esac
    # surfaces made children of themselves or of their descendants: refused, lord-server goes on
    sh tests/popup_headless.sh cycle-client >"$T/out" 2>&1
    case $? in
        0) ok "surface-cycles (tests/cycle_client.c, headless lord-server)" ;;
        77) skip "surface-cycles" "build/lord-server and the default theme (make, npm run build)" ;;
        *) bad "surface-cycles (tests/cycle_client.c, headless lord-server)"; cat "$T/out" ;;
    esac
    # hostile clients: random misuse of the requests whose misuse is a protocol error; lord-server answers every one,
    # a well-behaved window beside them is never disconnected and still gets its frames
    sh tests/popup_headless.sh wlfuzz-client >"$T/out" 2>&1
    case $? in
        0) ok "wayland-fuzz (tests/wlfuzz_client.c, headless lord-server)" ;;
        77) skip "wayland-fuzz" "build/lord-server and the default theme (make, npm run build)" ;;
        *) bad "wayland-fuzz (tests/wlfuzz_client.c, headless lord-server)"; cat "$T/out" ;;
    esac
    # a theme switch where the new theme and the running one both fail: the recovery screen, never no JS at all; and
    # one after a package update under the session: the running theme comes back, a changed unsigned manifest is never taken
    sh tests/theme_switch_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "theme-switch-fails (tests/theme_switch_headless.sh, headless lord-server)" ;;
        77) skip "theme-switch-fails" "build/lord-server (make)" ;;
        *) bad "theme-switch-fails (tests/theme_switch_headless.sh, headless lord-server)"; cat "$T/out" ;;
    esac
    # explicit sync's kernel side (src/wl/syncobj.c) on this machine's render node
    ./build/syncobj-kernel-test >"$T/out" 2>&1
    case $? in
        0) ok "syncobj-kernel-test (src/wl/syncobj.c: import, eventfd, release at once and with a dmabuf's fences)" ;;
        77) skip "syncobj-kernel-test" "a render node with timeline syncobjs and SYNCOBJ_EVENTFD (Linux 6.6)" ;;
        *) bad "syncobj-kernel-test (src/wl/syncobj.c)" ;;
    esac
    # explicit sync (linux-drm-syncobj-v1): timelines and udmabufs through the kernel, against a headless lord-server
    sh tests/syncobj_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "syncobj (tests/syncobj_client.c, headless lord-server: wait, order, freeze, limit, fair, errors)" ;;
        77) skip "syncobj" "a render node with timeline syncobjs, /dev/udmabuf (group kvm), build/lord-server and the default theme" ;;
        *) bad "syncobj (tests/syncobj_client.c, headless lord-server: wait, order, freeze, limit, fair, errors)" ;;
    esac
    # frames, titles and damage of a Linux window, against a headless lord-server (docs/WAYLAND.md)
    sh tests/wlperf_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "wlperf (tests/wlperf_client.c, headless lord-server: titles, frames, protocols, transform, fixes, subsurface, damage)" ;;
        77) skip "wlperf" "build/lord-server and the default theme (make, npm run build)" ;;
        *) bad "wlperf (tests/wlperf_client.c, headless lord-server: titles, frames, protocols, transform, fixes, subsurface, damage)" ;;
    esac
    # color-management-v1 and color-representation-v1, against a headless lord-server (docs/WAYLAND.md, "Colour management")
    sh tests/color_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "color (tests/color_client.c, headless lord-server: what is advertised, errors, linear light on screen)" ;;
        77) skip "color" "build/lord-server, build/color-client and the default theme (make, npm run build)" ;;
        *) bad "color (tests/color_client.c, headless lord-server: what is advertised, errors, linear light on screen)" ;;
    esac
    # fifo-v1, commit-timing-v1, tearing-control-v1, content-type-v1 against a headless lord-server, vkcube's FIFO if
    # installed (docs/WAYLAND.md, "Frame timing")
    sh tests/wltiming_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "wltiming (tests/wltiming_client.c, headless lord-server: fifo, commit timing, errors, vkcube)" ;;
        77) skip "wltiming" "build/lord-server and the default theme (make, npm run build)" ;;
        *) bad "wltiming (tests/wltiming_client.c, headless lord-server: fifo, commit timing, errors, vkcube)" ;;
    esac
    # the shell extras: tab tear-off errors, window tags, the bell, blur behind windows, session management, against a
    # headless lord-server (tests/shellx_headless.sh; the pointer-driven tear-off with SHELLX_DRAG=1, as root)
    if have node; then run "shellx-model-test (sdk/shellxmodel.js)" node tests/shellx_model_test.mjs; fi
    sh tests/shellx_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "shellx (tests/shellx_client.c, headless lord-server: globals, tag, bell, blur, session)" ;;
        77) skip "shellx" "build/lord-server, build/shellx-client and the default theme (make, npm run build)" ;;
        *) bad "shellx (tests/shellx_client.c, headless lord-server: globals, tag, bell, blur, session)" ;;
    esac
    # apps mode's guest side: lord-server's window endpoint, directly and through lordos-guest (tests/guestwin_client.c)
    sh tests/guestwin_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "guestwin (tests/guestwin_client.c, headless lord-server and lordos-guest, apps mode)" ;;
        77) skip "guestwin" "build/lord-server, build/lordos-guest and the default theme (make, npm run build)" ;;
        *) bad "guestwin (tests/guestwin_client.c, headless lord-server and lordos-guest, apps mode)" ;;
    esac
    # the machines command's session channel: start, open and poweroff of a throwaway machine through a headless
    # lord-server's Machines app, and the requests the channel refuses (tests/machinescli_headless.sh)
    sh tests/machinescli_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "machines-cli (tests/machinescli_headless.sh, headless lord-server, a throwaway machine)" ;;
        77) skip "machines-cli" "build/lord-server, build/machines, build/lord-vm, KVM, QEMU and the default theme (make, npm run build)" ;;
        *) bad "machines-cli (tests/machinescli_headless.sh, headless lord-server, a throwaway machine)" ;;
    esac
    # a machine group (Whonix) through a headless lord-server: start order, the link, Tor only through the gateway, no
    # network with the gateway off, relinked after its restart (tests/vm_group_link.sh; its disks are WHONIX_GW and
    # WHONIX_WS, docs/VIRTUALIZATION.md 19.0, and several minutes)
    bash tests/vm_group_link.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-group-link (tests/vm_group_link.sh, headless lord-server, a Whonix group)" ;;
        77) skip "vm-group-link" "WHONIX_GW and WHONIX_WS (Whonix's disks with a serial shell), KVM, QEMU, unshare and the default theme" ;;
        *) bad "vm-group-link (tests/vm_group_link.sh, headless lord-server, a Whonix group)"; tail -20 "$T/out" ;;
    esac
    # machines shell and console against a stand-in lord-vm (tests/machines_shell_test.sh)
    bash tests/machines_shell_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "machines-shell (tests/machines_shell_test.sh, machines shell and console against a stand-in lord-vm)" ;;
        77) skip "machines-shell" "build/machines, cc and python3" ;;
        *) bad "machines-shell (tests/machines_shell_test.sh, machines shell and console against a stand-in lord-vm)"; tail -20 "$T/out" ;;
    esac
    # machines exec, cp, logs and mcp against a real running Linux machine with automation on (tests/vm_exec_e2e.sh;
    # VM_EXEC_NAME and VM_EXEC_USER, in a private HOME and namespaces)
    bash tests/vm_exec_e2e.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-exec-e2e (tests/vm_exec_e2e.sh, machines exec, cp, logs and mcp in a real guest)" ;;
        77) skip "vm-exec-e2e" "VM_EXEC_NAME and VM_EXEC_USER (a running Linux machine with automation on), build/machines and python3" ;;
        *) bad "vm-exec-e2e (tests/vm_exec_e2e.sh, machines exec, cp, logs and mcp in a real guest)"; tail -20 "$T/out" ;;
    esac
    # a machine group's Delete, Rename and Repair, and a base made again from the verified archive, on throwaway groups
    # made from a fixture archive (tests/vm_group_manage.sh; docs/VIRTUALIZATION.md 19.7-19.9)
    bash tests/vm_group_manage.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-group-manage (tests/vm_group_manage.sh, lord-vm --group-delete/rename/repair/fresh on throwaway groups)" ;;
        77) skip "vm-group-manage" "build/lord-vm-testpaths, build/lord-vmunpack, build/machines, qemu-img, qemu-io, xz, tar, python3 and /dev/kvm" ;;
        *) bad "vm-group-manage (tests/vm_group_manage.sh, lord-vm --group-delete/rename/repair/fresh on throwaway groups)"; tail -20 "$T/out" ;;
    esac
    # the same through Machines' own calls in a headless lord-server, in its own namespaces (lord.vm.groups,
    # groupRenamePlan, groupRename, groupRepair, groupDelete; tests/machines_group_manage_headless.sh)
    bash tests/machines_group_manage_headless.sh >"$T/out" 2>&1
    case $? in
        0) ok "machines-group-manage (tests/machines_group_manage_headless.sh, headless lord-server, throwaway groups)" ;;
        77) skip "machines-group-manage" "build/lord-server, lord-vm, lord-vm-testpaths, lord-vmunpack, the default theme, /dev/kvm, unshare and an installed catalog and image store to mount over" ;;
        *) bad "machines-group-manage (tests/machines_group_manage_headless.sh, headless lord-server, throwaway groups)"; tail -20 "$T/out" ;;
    esac
    # Machines' storage under real QEMU/KVM (tests/vm_storage_test.sh): encrypted machines, both disposable sessions,
    # snapshot, clone, Export (.lordvm and OVA), the import of an export (tampered and hostile), raw disks
    bash tests/vm_storage_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-storage (tests/vm_storage_test.sh, encryption, disposable sessions, Export, raw disks)" ;;
        77) skip "vm-storage" "build/lord-vm, KVM, QEMU (qemu-img, qemu-io), swtpm, OVMF and python3" ;;
        *) bad "vm-storage (tests/vm_storage_test.sh, encryption, disposable sessions, Export, raw disks)"; tail -20 "$T/out" ;;
    esac
    # snapshots' kept settings (put back, a disk added refused, a running machine's hardware) and Reclaim space's
    # compression (tests/vm_snap_store_test.sh), real lord-vm, qemu-img and QEMU/KVM
    bash tests/vm_snap_store_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-snap-store (tests/vm_snap_store_test.sh, snapshots' settings, compression)" ;;
        77) skip "vm-snap-store" "build/lord-vm, KVM, QEMU (qemu-img, qemu-io), OVMF and python3" ;;
        *) bad "vm-snap-store (tests/vm_snap_store_test.sh, snapshots' settings, compression)"; tail -20 "$T/out" ;;
    esac
    # lordos-guest's clipboard tool (xclip, wl-clipboard, none: the hello says "noclip"), stub tools and a socket port
    sh tests/guest_clip_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "guest-clip (tests/guest_clip_test.sh, lordos-guest's clipboard tools)" ;;
        77) skip "guest-clip" "build/lordos-guest and python3" ;;
        *) bad "guest-clip (tests/guest_clip_test.sh, lordos-guest's clipboard tools)" ;;
    esac
    # lordos-guest --stopping: the guest's shutdown hook says power off or restart, waits for lord-vm's echo (QEMU issue 4918)
    sh tests/guest_stop_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "guest-stop (tests/guest_stop_test.sh, lordos-guest's shutdown notice)" ;;
        77) skip "guest-stop" "build/lordos-guest and python3" ;;
        *) bad "guest-stop (tests/guest_stop_test.sh, lordos-guest's shutdown notice)" ;;
    esac
    # lordos-guest 8: the agent's shell (pty, size, exit status, flow control, the host's close) and links and
    # notifications for the host through its local socket (6.12, 6.18)
    sh tests/guest_shell_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "guest-shell (tests/guest_shell_test.sh, lordos-guest's agent shell, links and notifications)" ;;
        77) skip "guest-shell" "build/lordos-guest and python3" ;;
        *) bad "guest-shell (tests/guest_shell_test.sh, lordos-guest's agent shell, links and notifications)" ;;
    esac
    # lordos-guest 9: "Send to LordOS" (6.21): the offer, flow control (it stops at the window), a resume after a lost
    # connection, a prefix that differs, "not now" and "off"
    sh tests/guest_send_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "guest-send (tests/guest_send_test.sh, lordos-guest's Send to LordOS)" ;;
        77) skip "guest-send" "build/lordos-guest and python3" ;;
        *) bad "guest-send (tests/guest_send_test.sh, lordos-guest's Send to LordOS)" ;;
    esac
    # lordos-guest's performance profile on private buses: the real dconf-service, stand-ins for power-profiles-daemon
    # and the portal (tests/guest_perf_mock.py)
    sh tests/guest_perf_test.sh >"$T/out" 2>&1
    case $? in
        0) ok "guest-perf (tests/guest_perf_test.sh, lordos-guest's performance profile over D-Bus)" ;;
        77) skip "guest-perf" "build/lordos-guest, python3 with gi, dbus-daemon, dconf-service and gsettings" ;;
        *) bad "guest-perf (tests/guest_perf_test.sh, lordos-guest's performance profile over D-Bus)" ;;
    esac
    # a getter that destroys the node lord.setProp / lord.setStyle are writing (Claude Security, 0.8.9)
    sh tests/js_getter_destroy.sh >"$T/out" 2>&1
    case $? in
        0) ok "js-getter-destroy (tests/js_getter_destroy.js, headless lord-server)" ;;
        77) skip "js-getter-destroy" "build/lord-server (make)" ;;
        *) bad "js-getter-destroy (tests/js_getter_destroy.js, headless lord-server)" ;;
    esac
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
    if have node; then run "vm-windows-test (apps/machines/windows.js)" node tests/vm_windows_test.mjs; fi
    if have node; then run "vm-capture-test (apps/machines/capture.js, Capture mouse)" node tests/vm_capture_test.mjs; fi
    if have node; then run "vm-alldisplays-test (apps/machines/alldisplays.js, All Displays)" node tests/vm_alldisplays_test.mjs; fi
    # Machines: crafted images the import must refuse (needs qemu-img and this computer's qemu-system)
    if have qemu-img && { have qemu-system-x86_64 || have qemu-system-aarch64; } && [ -x build/lord-vm ]; then run "vm-import-test" sh tests/vm_import_test.sh build
    else skip "vm-import-test" "qemu-img, qemu-system"; fi
    # Machines: renaming a throwaway machine (folder, transfer folder, snapshot, app.json; refusals; running with KVM)
    sh tests/vm_rename.sh >"$T/out" 2>&1
    case $? in
        0) ok "vm-rename (tests/vm_rename.sh, lord-vm --rename and machines rename on throwaway machines)" ;;
        77) skip "vm-rename" "build/lord-vm, build/machines, qemu-img and python3" ;;
        *) bad "vm-rename (tests/vm_rename.sh, lord-vm --rename and machines rename on throwaway machines)" ;;
    esac
    if have node; then run "screenshots-model-test (apps/screenshots)" node tests/screenshots_model_test.mjs; fi
    # the Screenshots toolbar (❖⇧5): options kept and read back, modes, keys, Save to, the remembered selection's handles
    if have node; then run "screenshots-toolbar-model-test (apps/screenshots/toolbarmodel.js)" node tests/screenshots_toolbar_model_test.mjs; fi
    # Settings → Privacy's rows: a built-in app that declares a permission is listed before it asked (GitHub #219)
    if have node; then run "privacy-model-test (apps/settings/privacymodel.js)" node tests/privacy_model_test.mjs; fi
    # the clipboard history panels: keys, scrolling to the selection only, a pointer at rest never takes it (GitHub #203, #204)
    if have node; then run "clipboard-panel-model-test (sdk/clipboardpanelmodel.js)" node tests/clipboard_panel_model_test.mjs; fi
    # the clipboard's Snippets: add, edit, rename, delete, reorder, search, the file and its limits (GitHub #205)
    if have node; then run "snippets-model-test (sdk/snippetsmodel.js)" node tests/snippets_model_test.mjs; fi
    if have node; then run "imageedit-model-test (sdk/imageeditmodel.js)" node tests/imageedit_model_test.mjs; fi
    if have node; then run "osk-model-test (sdk/oskmodel.js)" node tests/osk_model_test.mjs
    else skip "osk-model-test (sdk/oskmodel.js)" node; fi
    if have node; then run "motion-model-test (sdk/motionmodel.js)" node tests/motion_model_test.mjs; else skip "motion-model-test" node; fi
    if have node; then run "wallpaper-model-test (sdk/wallpapermodel.js)" node tests/wallpaper_model_test.mjs; else skip "wallpaper-model-test" node; fi
    if have node; then run "display-apps-model-test (sdk/displayappsmodel.js)" node tests/display_apps_model_test.mjs; else skip "display-apps-model-test" node; fi
    if have node; then run "slideshow-model-test (sdk/slideshowmodel.js)" node tests/slideshow_model_test.mjs; else skip "slideshow-model-test" node; fi
    if have node; then run "widget-grid-test (sdk/widgetgrid.js)" node tests/widget_grid_test.mjs; else skip "widget-grid-test" node; fi
    if have node; then run "timer-widget-test (sdk/timerwidgetmodel.js, every language)" node tests/timer_widget_test.mjs; else skip "timer-widget-test" node; fi
    if have node; then run "poll-same-test (sdk/samevalue.js)" node tests/poll_same_test.mjs; else skip "poll-same-test" node; fi
    # LordOS Studio (docs/CAMERA-STUDIO-PLAN.md): our own inference against ONNX Runtime's stored outputs and the loader's
    # fuzz; the frame pipeline on fixed frames (CPU, and GPU when a render node opens); the popover's plain logic
    run "studio-nn-test (segmenter and BlazeFace against ONNX Runtime, the .lnn loader's fuzz)" build/studio-nn-test
    run "studio-test (matte, guided filter, blur, picture, mirror; CPU and GPU agree)" build/studio-test
    run "studio-hub-test (the loopback: what it shows, its readers, the cards, YUYV; phase 3's settings and names)" build/studio-hub-test
    # phase 3: each look against the reference frames (Face Light, colour match, filters, green screen, video, Auto
    # Framing, Retouch; CPU and GPU agree), the background catalog's closed actions and the shipped catalog, the voice drop-in
    run "studio-fx-test (phase 3's effects on fixed frames)" build/studio-fx-test
    run "studio-bg-admin-test (studio-bg-get, studio-bg-remove, the catalog)" build/studio-bg-admin-test
    run "mok-admin-test (dkms-mok-enroll with fake firmware variables and a fake mokutil)" build/mok-admin-test
    if have node; then run "studio-voice-test (the lordos-voice.conf drop-in, Mic Mode)" node tests/studio_voice_test.mjs; else skip "studio-voice-test" node; fi
    # phase 2's package, built and inspected (never installed; the device itself is checked on a VM)
    if have dpkg-deb; then run "studio-package-test (lordos-studio-camera)" sh tests/studio_package_test.sh; else skip "studio-package-test" dpkg-deb; fi
    if have node; then run "studio-model-test (sdk/studiomodel.js)" node tests/studio_model_test.mjs; else skip "studio-model-test" node; fi
    # the background catalog at 4K: every entry, license, size, hash and resolution (the publish folder with LORDOS_STUDIO_STAGE)
    if have node; then run "studio-catalog-test (assets/studio/backgrounds.json, tools/studio/bg-sources.json)" node tests/studio_catalog_test.mjs; else skip "studio-catalog-test" node; fi
    if have node; then run "widget-feed-test (sdk/widgetfeedmodel.js)" node tests/widget_feed_test.mjs; else skip "widget-feed-test" node; fi
    if have node; then run "devices-model-test (sdk/devicesmodel.js)" node tests/devices_model_test.mjs; else skip "devices-model-test" node; fi
    if have node; then run "battery-policy-test (sdk/batterypolicy.js)" node tests/battery_policy_test.mjs; else skip "battery-policy-test" node; fi
    if have node; then run "sentinela-model-test (sdk/sentinelamodel.js, sdk/llmcatalog.js)" node tests/sentinela_model_test.mjs; else skip "sentinela-model-test" node; fi
    if have node; then run "sentinela-vm-test (sdk/sentinelavm.js: the machine rules)" node tests/sentinela_vm_test.mjs; else skip "sentinela-vm-test" node; fi
    # (0.8.9.6) Investigate with Lumo: the facts, the strict answer, the evidence, a translation fenced as data
    if have node; then run "sentinela-investigate-test (sdk/sentinelamodel.js: Investigate and Translate with Lumo)" node --no-warnings tests/sentinela_investigate_test.mjs; else skip "sentinela-investigate-test" node; fi
    # (0.8.9.6) Get Info's description: the sources' order, the language and the fallback to English
    if have node; then run "describe-model-test (sdk/describemodel.js)" node --no-warnings tests/describe_model_test.mjs; else skip "describe-model-test" node; fi
    if have node; then run "avatar-model-test (apps/settings/avatarmodel.js)" node tests/avatar_model_test.mjs; else skip "avatar-model-test" node; fi
    if have node; then run "weather-model-test (sdk/weathermodel.js)" node tests/weather_model_test.mjs; else skip "weather-model-test" node; fi
    if have node; then run "vpn-model-test (sdk/vpnmodel.js)" node tests/vpn_model_test.mjs; else skip "vpn-model-test" node; fi
    if have node; then run "claude-widget-model-test (sdk/claudewidgetmodel.js)" node tests/claude_widget_model_test.mjs; else skip "claude-widget-model-test" node; fi
    if have node; then run "stream-model-test (sdk/streammodel.js)" node tests/stream_model_test.mjs; else skip "stream-model-test" node; fi
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
    if have node; then run "flathub-model-test (sdk/flathubmodel.js)" node tests/flathub_model_test.mjs; fi
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
    # plural rules against CLDR and every bundled language's meta.json and format.json (sdk/plurals.js)
    if have node; then run "locales-test (sdk/plurals.js, assets/locales)" node tests/locales_test.mjs
    else skip "locales-test" "node"; fi
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
    # Settings → Network → Wi-Fi's lists: the connected network first, saved ones in range (GitHub #167)
    if have node; then run "wifi-model-test (apps/settings/wifimodel.js)" node tests/wifi_model_test.mjs
    else skip "wifi-model-test" "node"; fi
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
    if have node; then run "nv-report-test (apps/monitor/nvreport.js, Monitor → GPU's NVIDIA failure and report)" node tests/nv_report_test.mjs
    else skip "nv-report-test" "node"; fi
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
    # the column view: what is picked in a deeper column is selected, Select All takes its column (apps/files/colview.js, GitHub #100, #170)
    if have node; then run "files-colview-test (apps/files/colview.js)" node tests/files_colview_test.mjs
    else skip "files-colview-test" "node"; fi
    # the rubber band: the rectangle, which cards and rows it touches, Shift and Ctrl, autoscroll, right to left, one update per frame (apps/files/marquee.js, GitHub #233)
    if have node; then run "files-marquee-test (apps/files/marquee.js)" node tests/files_marquee_test.mjs
    else skip "files-marquee-test" "node"; fi
    # the slow second click that renames in place: double click, too late, drag, moved pointer, modifiers, selection, focus (sdk/slowclick.js)
    if have node; then run "slowclick-test (sdk/slowclick.js)" node tests/slowclick_test.mjs
    else skip "slowclick-test" "node"; fi
    # color tags and comments: the last tag and an emptied comment are removed (apps/files/tags.js, GitHub #181)
    if have node; then run "files-tags-test (apps/files/tags.js)" node tests/files_tags_test.mjs
    else skip "files-tags-test" "node"; fi
    # the free names of copies, duplicates and new folders fit 255 bytes (apps/files/names.js, GitHub #102)
    if have node; then run "files-names-test (apps/files/names.js)" node tests/files_names_test.mjs
    else skip "files-names-test" "node"; fi
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
    # the FileChooser portal's dialog: the app's glob filters (Chromium's "*.[pP][dD][fF]") and where it goes (sdk/portalfilemodel.js)
    if have node; then run "portal-file-model-test (sdk/portalfilemodel.js)" node tests/portal_file_model_test.mjs
    else skip "portal-file-model-test" "node"; fi
    # a Chromium-family browser's profiles from its own Local State (untrusted) and the profile an activity or display picks
    if have node; then run "browser-profiles-test (sdk/browserprofilemodel.js)" node tests/browser_profiles_test.mjs
    else skip "browser-profiles-test" "node"; fi
    # (0.8.8.8) a Linux app's global shortcuts: the sheet's rows, the combination a key names, what is refused (sdk/globalshortcutsmodel.js)
    if have node; then run "global-shortcuts-model-test (sdk/globalshortcutsmodel.js)" node tests/global_shortcuts_model_test.mjs
    else skip "global-shortcuts-model-test" "node"; fi
    # (0.8.8.8) a Linux app's ScreenCast chooser: what is offered, one or several, what lord-server gets (sdk/screencastmodel.js)
    if have node; then run "screencast-model-test (sdk/screencastmodel.js)" node tests/screencast_model_test.mjs
    else skip "screencast-model-test" "node"; fi
    # (in development) the capture indicator by the clock: its glyph, one row per app, what Stop and Turn off do (sdk/capturemodel.js)
    if have node; then run "capture-model-test (sdk/capturemodel.js)" node tests/capture_model_test.mjs
    else skip "capture-model-test" "node"; fi
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
    # (0.8.9.6) Monitor → Processes: Grouped, the steady list, who started it and the origin badges (apps/monitor/procgroupmodel.js)
    if have node; then run "monitor-procgroup-test (apps/monitor/procgroupmodel.js, seal.js)" node tests/monitor_procgroup_test.mjs
    else skip "monitor-procgroup-test" "node"; fi
    # (Machines to 100 %) a running machine's own disk and network numbers on its row (apps/monitor/vmiomodel.js)
    if have node; then run "monitor-vmio-test (apps/monitor/vmiomodel.js)" node tests/monitor_vmio_test.mjs
    else skip "monitor-vmio-test" "node"; fi
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
    # (after 0.8.9.5) the Store's installed apps by package system (apps/store/installedmodel.js): the filters, the systems
    if have node; then run "store-installed-test (apps/store/installedmodel.js)" node tests/store_installed_test.mjs
    else skip "store-installed-test" "node"; fi
    # (after 0.8.9.5) Store → Apps LordOS, LordOS's own store (apps/store/lordosmodel.js, assets/store/lordos-apps.json)
    if have node; then run "store-lordos-test (apps/store/lordosmodel.js)" node tests/store_lordos_test.mjs
    else skip "store-lordos-test" "node"; fi
    # (0.8.7) what the live ISO opens by itself, the "Check this computer" boot entry and the menu's height
    if have node; then run "livestart-test (sdk/livestart.js, the ISO's boot menu)" node tests/livestart_test.mjs
    else skip "livestart-test" "node"; fi
    # (0.8.5) Settings → Displays' arrangement (sdk/displaylayout.js): numbers, magnetic snap, valid drops
    if have node; then run "display-layout-test (sdk/displaylayout.js)" node tests/display_layout_test.mjs
    else skip "display-layout-test" "node"; fi
    # (0.8.6) runtime/index.js's skip of unchanged styles sends the same lord.* calls as without it
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "runtime-props-test (runtime/index.js)" node tests/runtime_props_test.mjs
        # (#234) a mouse's back and forward buttons: a navigate event for the native window under the pointer
        run "navbuttons-test (runtime/events.js, sdk/navmodel.js)" node tests/navbuttons_test.mjs
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
    # (#238) Notes' storage over a real temporary home: the format, titles, geometry, attachments (odd names, links,
    # folders, a huge file) and what a link or image may reach (file://, javascript:, absolute paths, ..); writes stay in ~/Notes
    if have node && [ -d ../node_modules/esbuild ]; then run "notes-store-test (apps/notes/store.js, links.js)" node tests/notes_store_test.mjs
    else skip "notes-store-test" "node and the repository's node_modules (npm install)"; fi
    if have node; then run "sysopen-untrusted-test (sdk/runscode.js: attachments that would run never reach a Linux app)" node tests/sysopen_untrusted_test.mjs; else skip "sysopen-untrusted-test" node; fi
    # (0.8.8.8, #117) Notes: Edit and Format menus and the editor's right-click menu send the editor's own keys
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "notes-editor-scene (apps/notes/NoteEditor.jsx, editmenu.js)" node tests/textfield_menu_test.mjs notes_editor_scene.mjs
        # (#169) Notes' reading view keeps a paragraph's line breaks, as the editor shows them (sdk/markdown.jsx breaks)
        run "notes-breaks-scene (sdk/markdown.jsx breaks, apps/notes)" node tests/textfield_menu_test.mjs notes_breaks_scene.mjs
        # (#166) Screenshots' markup editor: New Screenshot and Ctrl+N bring the capture panel back, the editor staying open
        run "screenshots-newcapture-scene (apps/screenshots/editor.jsx)" node tests/textfield_menu_test.mjs screenshots_newcapture_scene.mjs
        # (#252) Screenshots with several displays: Print Screen copies the main display's capture, a thumbnail per display
        # inside it, the markup editor inside one display (side by side, main listed second; another above-left at 200 %)
        run "screenshots-displays-scene (apps/screenshots/index.jsx)" node tests/textfield_menu_test.mjs screenshots_displays_scene.mjs
        # the Screenshots toolbar: modes, the primary button, the Options menu and its choices saved in settings
        run "screenshots-toolbar-scene (apps/screenshots/toolbar.jsx)" node tests/textfield_menu_test.mjs screenshots_toolbar_scene.mjs
        # (#200) Backups: Choose… for the destination (no Cloud folder) and the exclusions (inside home), typing still works
        run "backup-pickers-scene (apps/backup/index.jsx, FilePicker cloud={false})" node tests/textfield_menu_test.mjs backup_pickers_scene.mjs
        # (#231) Backups' restore browser: a file's icon isn't an empty check box, a long name ends with … on Restore
        run "backup-restore-scene (apps/backup/index.jsx, sdk Button)" node tests/textfield_menu_test.mjs backup_restore_scene.mjs
    else skip "notes-editor-scene" "node and the repository's node_modules (npm install)"; fi
    # a password TextInput: the eye shows it and keeps the focus, copies nothing, hides on blur; ⇪ follows Caps Lock
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "password-field-scene (sdk/components.jsx TextInput secure)" node tests/textfield_menu_test.mjs password_field_scene.mjs
    else skip "password-field-scene" "node and the repository's node_modules (npm install)"; fi
    # (#205) the clipboard's Snippets tab: search, paste, From clipboard, Save as snippet, reorder, delete, no password kept
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "clipboard-snippets-scene (sdk/clipboard.js FullPanel, sdk/snippetsmodel.js)" node tests/textfield_menu_test.mjs clipboard_snippets_scene.mjs
    else skip "clipboard-snippets-scene" "node and the repository's node_modules (npm install)"; fi
    # every password surface LordOS draws is protected (black in captures): password fields, the keyring and polkit prompts
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "password-protect-scene (TextInput secure, KeyringPrompt, PolkitPrompt)" node tests/textfield_menu_test.mjs password_protect_scene.mjs
    else skip "password-protect-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9.5, docs/MAIN-THREAD.md) <Toggle pending> and <Button pending> answer the press before the action, flip back on a
    # failure; useTrackedSettings renders only for the keys read; ThemeProvider keeps equal tokens; sortByName's order
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "pending-scene (sdk/components.jsx Toggle/Button pending, sdk/settings.js, sdk/theme.js, sdk/collate.js)" node tests/textfield_menu_test.mjs pending_scene.mjs
    else skip "pending-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) the ScreenCast chooser: the app's name, the pictures, Share only with a pick, Cancel, the app closing it (sdk/screencast.jsx)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "screencast-scene (sdk/screencast.jsx)" node tests/textfield_menu_test.mjs screencast_scene.mjs
    else skip "screencast-scene" "node and the repository's node_modules (npm install)"; fi
    # LordOS Studio's ways in: the capture panel's Mic Mode entry with the microphone only (no preview), Video effects
    # with the camera, the LordOS Studio app's window and its launcher listing (sdk/capturestatus.jsx, sdk/studio.jsx)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "capture-studio-scene (sdk/capturestatus.jsx, sdk/studio.jsx, apps/studio)" node tests/textfield_menu_test.mjs capture_studio_scene.mjs
    else skip "capture-studio-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) the Store's "Add from an address" sheet: https only, the origin lord-web read, Install sends no URL
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "webapps-sheet-scene (apps/store/addweb.js)" node tests/textfield_menu_test.mjs webapps_sheet_scene.mjs
    else skip "webapps-sheet-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) Get of a web app without lordos-web: the engine's plan and sizes, the package job, then the app with no click
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "webapps-get-scene (apps/store/getweb.js)" node tests/textfield_menu_test.mjs webapps_get_scene.mjs
    else skip "webapps-get-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.9) the trademark notice of the Store's Web apps section and of each web app's page
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "store-trademark-scene (apps/store/trademark.js)" node tests/textfield_menu_test.mjs store_trademark_scene.mjs
        run "battery-settings-scene (apps/settings/Battery.jsx)" node tests/textfield_menu_test.mjs battery_settings_scene.mjs
        # Sentinela (docs/SENTINELA.md): a rule's desktop notification with the closed actions, and its Settings page
        run "sentinela-notify-scene (sdk/sentinela.js)" node tests/textfield_menu_test.mjs sentinela_notify_scene.mjs
        run "sentinela-settings-scene (apps/settings/Sentinela.jsx)" node tests/textfield_menu_test.mjs sentinela_settings_scene.mjs
    else skip "store-trademark-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9.5) the Store's long lists load their next page near the end of the scroll
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "store-endless-scene (apps/store/endless.js)" node tests/textfield_menu_test.mjs store_endless_scene.mjs
    else skip "store-endless-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9.5) a Store card's one-line texts end in an ellipsis, never cut through a letter
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "store-card-scene (apps/store/ui.js)" node tests/textfield_menu_test.mjs store_card_scene.mjs
    else skip "store-card-scene" "node and the repository's node_modules (npm install)"; fi
    # (after 0.8.9.5) an origin's second column: Installed first, then its categories; the general Installed page's filters
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "store-origin-scene (apps/store/origincol.js)" node tests/textfield_menu_test.mjs store_origin_scene.mjs
    else skip "store-origin-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) Tab between the SDK's controls, the focus ring, Space and Enter, the arrows, a press that ends the focus (sdk/focus.js)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "focus-scene-test (sdk/focus.js, sdk/components.jsx, sdk/controls.jsx)" node tests/focus_scene_test.mjs
    else skip "focus-scene-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) the List, the Table, Tabs, Dropdown, RadioGroup, a Popover's focus and the wheel on a Slider, from the keys
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "list-scene (sdk/lists.jsx, sdk/controls.jsx)" node tests/focus_scene_test.mjs list_scene.mjs
    else skip "list-scene" "node and the repository's node_modules (npm install)"; fi
    # (security) a key typed while another window, a full-screen window's neighbour or a system prompt has the keyboard
    # never reaches a virtual machine's guest (wm.hasKeyboard, AppHost, apps/machines/model.js guestKeys)
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "vm-keys-scene (sdk/components.jsx AppHost, sdk/windows.js hasKeyboard, apps/machines guestKeys)" node tests/focus_scene_test.mjs vm_keys_scene.mjs
    else skip "vm-keys-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8) menus from the keyboard in both themes: arrows, type-ahead, submenus, the hover pause, F10 and ⌥ alone
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "menu-keys-test (themes/default Menu, themes/classic Menu, sdk/shortcuts.js)" node tests/menu_keys_test.mjs
    else skip "menu-keys-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #48) Lord Classic's power menu: a click on Lock Screen or Switch User reaches the session
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-startmenu-power-test (themes/classic StartMenu)" node tests/classic_startmenu_power_test.mjs
    else skip "classic-startmenu-power-test" "node and the repository's node_modules (npm install)"; fi
    # (#210) Lord Classic's start menu by category: the groups and counts, mouse and keyboard, RTL, the remembered view
    if have node; then run "app-groups-model-test (sdk/appgroupsmodel.js)" node tests/app_groups_model_test.mjs; fi
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-startmenu-categories-scene (themes/classic StartMenu, sdk/appgroupsmodel.js)" node tests/classic_startmenu_power_test.mjs classic_startmenu_categories_scene.mjs
    else skip "classic-startmenu-categories-scene" "node and the repository's node_modules (npm install)"; fi
    # (#159) a mouse's back and forward buttons go to the Linux app, never to the desktop's own press handlers
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "side-buttons-test (runtime/events.js)" node tests/side_buttons_test.mjs
    else skip "side-buttons-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6, #58) what the bar shows: Lord Flow's Control Center tiles, the output list, the clock's calendar
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "menubar-test (themes/default ControlCenter, NotificationCenter, MenuBars on every display, sdk/soundoutputs.js)" node tests/menubar_test.mjs
    else skip "menubar-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.8, #99) Files' column view: a tagged item shows its tag dots after the name, as the list view does;
    # (#133) every column down the chain is drawn, none dropped to make room
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "files-colview-scene (apps/files/views.jsx ColumnView)" node tests/menubar_test.mjs files_colview_scene.mjs
    else skip "files-colview-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9.6) Monitor → Processes grouped: the groups, a hover summary, Quit All on a snapshot, a child's Quit, the badges,
    # the chain and the held order
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "monitor-grouped-scene (apps/monitor/Processes.jsx)" node tests/menubar_test.mjs monitor_grouped_scene.mjs
    else skip "monitor-grouped-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9.6) Monitor → Processes: opening a group of 300 programs draws only the rows in sight, a few per pass, within
    # a frame (the desktop froze for a second), and a redraw keeps the rows whose values didn't change
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "monitor-freeze-scene (sdk/lists.jsx Table sight, apps/monitor/Processes.jsx)" node tests/menubar_test.mjs monitor_freeze_scene.mjs
    else skip "monitor-freeze-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9.6) Get Info's Description and Investigate with Lumo: only on the click, the closed actions, Blocklist asks first
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "monitor-describe-scene (apps/monitor/Describe.jsx)" node tests/menubar_test.mjs monitor_describe_scene.mjs
    else skip "monitor-describe-scene" "node and the repository's node_modules (npm install)"; fi
    # (#218) Files: dragging in a folder of 2000 files draws only the ghost and the drop highlights, never the whole window
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "files-drag-scene (apps/files/dnd.jsx)" node tests/menubar_test.mjs files_drag_scene.mjs
    else skip "files-drag-scene" "node and the repository's node_modules (npm install)"; fi
    # (0.8.8.6) Lord Classic's taskbar on every display: three monitors, panels on the clicked bar's display, one timer
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "classic-taskbar-test (themes/classic on every display, sdk/dock.js drop targets)" node tests/classic_taskbar_test.mjs
    else skip "classic-taskbar-test" "node and the repository's node_modules (npm install)"; fi
    # (0.8.9) Lord Workspaces: three monitors, tiling per display, Save layout, a monitor unplugged and back, the styles
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        run "workspaces-test (themes/workspaces on three displays)" node tests/workspaces_test.mjs
    else skip "workspaces-test" "node and the repository's node_modules (npm install)"; fi
    if have node; then run "workspaces-model-test (themes/workspaces/model, tokens.js, sdk/thememeta.js)" node tests/workspaces_model_test.mjs; fi
    # (0.8.9) the first-start desktop chooser: who sees it, the random order, the steps across the live theme switch,
    # and the whole wizard drawn over each theme (sdk/firststart.jsx)
    if have node; then run "firststart-model-test (sdk/firststartmodel.js)" node tests/firststart_model_test.mjs; fi
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        for th in default classic workspaces; do
            run "firststart-scene-$th (sdk/firststart.jsx over themes/$th)" node tests/workspaces_test.mjs firststart_scene.mjs "$th"
        done
    else skip "firststart-scene" "node and the repository's node_modules (npm install)"; fi
    # always on top in every theme: drawing order apart from the keyboard, the window menu's item, skip pager
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        for th in default classic workspaces celeste; do
            run "always-on-top-scene-$th (sdk/desktopmodel.js stackOrder over themes/$th)" node tests/workspaces_test.mjs always_on_top_scene.mjs "$th"
        done
    else skip "always-on-top-scene" "node and the repository's node_modules (npm install)"; fi
    # (#151, #219) title bar sizes, ❖ + drag anywhere in a window, the Window menu's tiling switch, in every theme
    if have node && [ -d ../node_modules/esbuild ] && [ -d ../node_modules/react-reconciler ]; then
        for th in default classic workspaces celeste; do
            run "window-requests-scene-$th (sdk/moddrag.js, sdk/windowbuttons.js over themes/$th)" node tests/workspaces_test.mjs window_requests_scene.mjs "$th"
        done
    else skip "window-requests-scene" "node and the repository's node_modules (npm install)"; fi
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
    # (in development) lord-web's view backend alone (src/web/webback.h, legacy or WPEPlatform, the one mk/web.mk built):
    # a local page's pixels, a click, keys and the wheel reaching its listeners, a resize, scale 2, hidden and shown
    if [ -x build/web-backend-test ] && have bwrap && have xdg-dbus-proxy; then
        run "web-backend-test ($(cat build/gen/web_backend 2>/dev/null) backend)" build/web-backend-test
    else skip "web-backend-test" "wpe-webkit-2.0 (build/web-backend-test), bubblewrap and xdg-dbus-proxy"; fi
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
    # LordOS Studio's helper without a camera: the pipeline inside its sandbox (GPU too), a non-camera fd, another job
    # and a damaged model refused
    if [ -x build/lord-studio ] && [ -x build/studio-helper-test ]; then run "studio-helper-test (lord-studio)" build/studio-helper-test build/lord-studio
    else skip "studio-helper-test" "build/lord-studio (FFmpeg -dev packages)"; fi
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
    # build whose check always fails, the way out to libx264 (only where a card encodes H.264). Every render node
    # goes in, as lord-server sends them: the first one is not always the card that encodes (an Intel iGPU
    # without its VA driver next to an AMD card)
    if [ -x build/lord-media ] && [ -x build/lmstream-test ]; then
        run "lmstream-test (libx264)" build/lmstream-test build/lord-media
        nodes=$(ls /dev/dri/renderD* 2>/dev/null | head -3)
        if [ -n "$nodes" ] && [ -x build/lord-media-encfail ]; then
            # shellcheck disable=SC2086
            run "lmstream-test (VA-API)" build/lmstream-test build/lord-media $nodes
            # shellcheck disable=SC2086
            run "lmstream-test (VA-API fails the check)" build/lmstream-test build/lord-media-encfail $nodes
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
    # Files' slow second click that renames in place (sdk/slowclick.js), driven as a mouse in a headless lord-server, in
    # every desktop: the icon, list and column views, and the clicks that must not rename (about a minute each)
    for th in default classic workspaces celeste; do
        SCENE_TIMEOUT=300 sh tests/app_scene_headless.sh files_slowrename_scene "$T/scene" "$th" >"$T/out" 2>&1
        case $? in
            0) ok "files-slowrename-scene $th (tests/files_slowrename_scene, headless lord-server)" ;;
            77) skip "files-slowrename-scene $th" "build/lord-server and themes/$th/theme.bundle.js (make, npm run build)" ;;
            *) bad "files-slowrename-scene $th (tests/files_slowrename_scene, headless lord-server)" ;;
        esac
    done
    # (0.8.9.6.1) Files' rename field takes the name's place without moving the icon grid, in every desktop and view
    for th in default classic workspaces celeste; do
        SCENE_TIMEOUT=100 sh tests/app_scene_headless.sh files_renamelayout_scene "$T/scene" "$th" >"$T/out" 2>&1
        case $? in
            0) ok "files-renamelayout-scene $th (tests/files_renamelayout_scene, headless lord-server)" ;;
            77) skip "files-renamelayout-scene $th" "build/lord-server and themes/$th/theme.bundle.js (make, npm run build)" ;;
            *) bad "files-renamelayout-scene $th (tests/files_renamelayout_scene, headless lord-server)" ;;
        esac
    done
    # (0.8.9.6.1) Lord Workspaces' top band: the activity strip never over the status cluster (Arabic at 1600 px, German
    # on a narrow screen), the accent under the activity in view
    for c in "ar 1600x900" "de 1024x768" "en 1280x800"; do
        lg=${c% *} sz=${c#* }
        SCENE_TIMEOUT=40 SCENE_SIZE=$sz sh tests/app_scene_headless.sh ws_topbar_scene "$T/scene" workspaces "$lg" >"$T/out" 2>&1
        case $? in
            0) ok "ws-topbar-scene $lg $sz (tests/ws_topbar_scene, headless lord-server)" ;;
            77) skip "ws-topbar-scene $lg $sz" "build/lord-server and themes/workspaces/theme.bundle.js (make, npm run build)" ;;
            *) bad "ws-topbar-scene $lg $sz (tests/ws_topbar_scene, headless lord-server)" ;;
        esac
    done
}

case $mode in
    quick) quick ;;
    full) full ;;
    *) echo "usage: sh tests/run_suites.sh [full]" >&2; exit 2 ;;
esac
printf '== %d passed, %d failed, %d skipped\n' "$pass" "$fails" "$skips"
[ "$fails" -eq 0 ] || { echo "failed: $failed"; exit 1; }
