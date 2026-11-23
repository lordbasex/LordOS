#!/bin/sh
# files_info_nofreeze.sh — Files → Get Info on a big folder, twice, in a headless lord-server: the test home holds
# 100 000 small files (tests/perf/seed.mjs; FILES_INFO_FILES changes it) and Get Info shows the home folder. Its size was
# lord.fs.du on the main thread (up to 900 ms on a big folder; 2026-11-23, docs/MAIN-THREAD.md). Fails when a size is
# measured on the main thread again (a "stall: … call fs.du" line), when the card never shows the size, or when opening it
# keeps the main thread busy for longer than FILES_INFO_STALL_MS (250 ms by default: the card renders in about 10–30 ms).
# Exits with 77 without build/lord-server or the default theme. About 30 s. Run from server/ (tests/run_suites.sh).
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/default/theme.bundle.js" ] && command -v node >/dev/null 2>&1 || exit 77
T=$(mktemp -d "${TMPDIR:-/tmp}/lnofreeze.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
PERF_FLATPAK_FILES=${FILES_INFO_FILES:-100000} PERF_OUT="$T/stalls" PERF_RAW="$T/raw" PERF_STEPS=1 \
    sh tests/perf_headless.sh default filesInfo >"$T/table" 2>&1
cat "$T/table"
node - "$T/raw" "$T/stalls" "${FILES_INFO_STALL_MS:-250}" <<'EOF'
const fs = require('node:fs');
const [raw, stalls, limit] = process.argv.slice(2);
const lines = fs.readFileSync(raw, 'utf8').split('\n').map((l) => { const i = l.indexOf('PERF '); try { return i < 0 ? null : JSON.parse(l.slice(i + 5)); } catch { return null; } }).filter(Boolean);
let fails = 0;
const check = (what, ok, detail = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? `: ${detail}` : ''}`); if (!ok) fails++; };
check('the run ended', lines.some((o) => o.done));
const du = fs.existsSync(stalls) ? fs.readFileSync(stalls, 'utf8').split('\n').filter((l) => /stall: [\d.]+ ms call fs\.du\b/.test(l)) : [];
check('no folder size measured on the main thread', du.length === 0, du[0] || '');
const opens = lines.filter((o) => o.step && /^open info/.test(o.step));
const worst = Math.max(0, ...opens.map((o) => o.max || 0));
check(`opening Get Info: longest run under ${limit} ms`, opens.length === 2 && opens.every((o) => o.ok) && worst < Number(limit), `${worst} ms`);
const shown = lines.filter((o) => o.infoSizes).flatMap((o) => o.infoSizes);
check('the card shows the size once measured', shown.some((s) => /\d ?[KMGT]?B · \d/.test(s)), JSON.stringify(shown.slice(0, 4)));
process.exit(fails ? 1 : 0);
EOF
