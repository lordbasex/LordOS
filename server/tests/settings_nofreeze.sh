#!/bin/sh
# settings_nofreeze.sh — Settings → Flatpak opened twice in a headless lord-server, with 200 000 files in the test home's
# ~/.local/share/flatpak and ~/.var/app (tests/perf/seed.mjs): measuring them froze the whole desktop for up to 2 s
# (maintainer, 2026-11-23; docs/MAIN-THREAD.md). Fails when a size is measured on the main thread again (a
# "stall: … call fs.du" line), when the page never shows a size, or when opening it keeps the main thread busy for
# longer than SETTINGS_STALL_MS (250 ms by default: the page itself renders in about 15–30 ms). Exits with 77 without
# build/lord-server or the default theme. About 40 s. Run from server/ (tests/run_suites.sh).
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/default/theme.bundle.js" ] && command -v node >/dev/null 2>&1 || exit 77
T=$(mktemp -d "${TMPDIR:-/tmp}/lnofreeze.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
PERF_FLATPAK_FILES=${PERF_FLATPAK_FILES:-200000} PERF_OUT="$T/stalls" PERF_RAW="$T/raw" PERF_STEPS=1 \
    sh tests/perf_headless.sh default flatpak >"$T/table" 2>&1
cat "$T/table"
node - "$T/raw" "$T/stalls" "${SETTINGS_STALL_MS:-250}" <<'EOF'
const fs = require('node:fs');
const [raw, stalls, limit] = process.argv.slice(2);
const lines = fs.readFileSync(raw, 'utf8').split('\n').map((l) => { const i = l.indexOf('PERF '); try { return i < 0 ? null : JSON.parse(l.slice(i + 5)); } catch { return null; } }).filter(Boolean);
let fails = 0;
const check = (what, ok, detail = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? `: ${detail}` : ''}`); if (!ok) fails++; };
check('the run ended', lines.some((o) => o.done));
const du = fs.existsSync(stalls) ? fs.readFileSync(stalls, 'utf8').split('\n').filter((l) => /stall: [\d.]+ ms call fs\.du\b/.test(l)) : [];
check('no folder size measured on the main thread', du.length === 0, du[0] || '');
const opens = lines.filter((o) => o.step && /^open flatpak/.test(o.step));
const worst = Math.max(0, ...opens.map((o) => o.max || 0));
check(`opening the page: longest run under ${limit} ms`, opens.length === 2 && worst < Number(limit), `${worst} ms`);
const shown = lines.filter((o) => o.flatpakSizes).flatMap((o) => o.flatpakSizes);
check('the page shows a size once measured', shown.some((s) => /\d.*B\+?$/.test(s)), JSON.stringify(shown.slice(0, 6)));
process.exit(fails ? 1 : 0);
EOF
