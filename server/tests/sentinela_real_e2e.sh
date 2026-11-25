#!/bin/sh
# sentinela_real_e2e.sh — one real Sentinela analysis without a session (docs/SENTINELA.md, "Tests"): a fake miner (a
# two-thread busy loop named kworkerd, run from a home-like folder) is started, its facts are read from /proc, the
# summary and the instructions are made by sdk/sentinelamodel.js, LordOS's llama-server analyzes it in lord-llm's
# sandbox (build/llm-engine-test --real, cold then warm), and the answer is checked by parseVerdict. Not part of
# `make test`: it needs /usr/bin/llama-server and a model.
#   tests/sentinela_real_e2e.sh <models folder> <id> [language name]    (the model: <folder>/<id>.gguf)
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <models folder> <id> [language]" >&2; exit 2; }
cd "$(dirname "$0")/.."
make -s build/lord-llm build/llm-engine-test >/dev/null
W=$(mktemp -d /tmp/sentinela-e2e-XXXXXX)
trap 'kill $MINER 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir -p "$W/home/.cache/x"
printf '#include <pthread.h>\nstatic void *spin(void *a){ volatile unsigned long n=0; for(;;) n++; return a; }\nint main(void){ pthread_t t; pthread_create(&t,0,spin,0); spin(0); }\n' > "$W/m.c"
cc -O0 -o "$W/home/.cache/x/kworkerd" "$W/m.c" -lpthread
sh -c "exec '$W/home/.cache/x/kworkerd'" &
MINER=$!
sleep 3
node --input-type=module - "$MINER" "$W" "${3:-Spanish}" <<'EOF'
import fs from 'node:fs';
const [pid, w, lang] = process.argv.slice(2);
const { buildSummary, systemPrompt, placeOf } = await import(new URL('../sdk/sentinelamodel.js', `file://${process.cwd()}/`).href);
const stat = (p) => { const t = fs.readFileSync(`/proc/${p}/stat`, 'utf8'); const r = t.lastIndexOf(')'); const f = t.slice(r + 2).split(' ');
  return { name: t.slice(t.indexOf('(') + 1, r), ppid: Number(f[1]), utime: Number(f[11]), stime: Number(f[12]), threads: Number(f[17]) }; };
const s0 = stat(pid);
await new Promise((r) => setTimeout(r, 2000));
const s1 = stat(pid);
const cpu = (s1.utime + s1.stime - s0.utime - s0.stime) / 100 / 2;
const exe = fs.readlinkSync(`/proc/${pid}/exe`);
const chain = [];
for (let p = s1.ppid, i = 0; p > 1 && i < 4; i++) { const s = stat(p); chain.push({ name: s.name, pkg: null }); p = s.ppid; }
const home = `${w}/home`;
const ev = { rule: 'cpu', pid: Number(pid), name: s1.name, exe, cpuAvg: cpu, cpuPeak: cpu, minutes: 5, rss: 2 << 20, threads: s1.threads };
const facts = { ok: true, exe, deleted: false, pkg: null, place: placeOf(exe, home), exeUid: 1000, exeWritable: true, chain };
fs.writeFileSync(`${w}/summary.txt`, buildSummary(ev, facts, { home, user: 'nobody', cores: 8, language: lang }));
fs.writeFileSync(`${w}/system.txt`, systemPrompt(lang));
console.log(`--- summary (${fs.readFileSync(`${w}/summary.txt`, 'utf8').length} characters)\n${fs.readFileSync(`${w}/summary.txt`, 'utf8')}`);
EOF
./build/llm-engine-test --real "$1" "$2" "$W/summary.txt" "$W/system.txt" | tee "$W/out.txt"
node --input-type=module - "$W/out.txt" <<'EOF'
import fs from 'node:fs';
const { parseVerdict } = await import(new URL('../sdk/sentinelamodel.js', `file://${process.cwd()}/`).href);
for (const line of fs.readFileSync(process.argv[2], 'utf8').split('\n').filter((l) => /answer /.test(l))) {
  const j = JSON.parse(line.slice(line.indexOf('{')));
  const v = j.ok ? parseVerdict(j.content) : null;
  console.log(v ? `parsed: ${JSON.stringify(v)} (${j.ms} ms, engine start ${j.startMs} ms, ${j.tokens} tokens)` : `REJECTED: ${line}`);
  if (!v) process.exitCode = 1;
}
EOF
