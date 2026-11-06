// files.js — Files' operations through lord.fs on one account (BASE: a folder of ~/Cloud, e.g. "MinIO/e2e-alpha").
// Replaced by files.sh: __BASE__, __TAG__. Returns what happened at each step; files.sh checks the server side.
const base = C('__BASE__'), T = `${base}/e2e-__TAG__`;
const out = { steps: [] };
const step = async (name, fn) => { const t = Date.now(); try { const v = await fn(); out.steps.push([name, 'ok', Date.now() - t, v]); return v; } catch (e) { out.steps.push([name, 'FAIL', Date.now() - t, String(e && (e.code || e.message) || e)]); return null; } };
const st = (p) => (cloud.status([p])[p] || {}).st;
const seen = async (p, want, ms = 90000) => { const s = []; const ok = await until(() => { const v = st(p); if (s[s.length - 1] !== v) s.push(v); return v === want; }, ms, 100); return { ok: !!ok, seen: s }; };
const job = async (id, ms = 600000) => { const j = await until(() => (lord.fs.jobs() || []).find((x) => x.id === id && x.done), ms, 200); if (j) try { lord.fs.jobForget(id); } catch (e) { /* gone */ } return j ? { failed: j.failed, error: j.error, bytes: j.bytes } : 'timeout'; };
await step('mkdir', () => lord.fs.mkdir(T));
await step('write a.txt', () => lord.fs.write(`${T}/a.txt`, 'hello from e2e __TAG__\n'));
await step('a.txt goes up (status)', () => seen(`${T}/a.txt`, 'here'));
await step('rename a.txt -> b.txt', () => lord.fs.rename(`${T}/a.txt`, `${T}/b.txt`));
await step('mkdir sub', () => lord.fs.mkdir(`${T}/sub`));
await step('move b.txt into sub', () => job(lord.fs.copy(`${T}/b.txt`, `${T}/sub/b.txt`, true)));
await step('copy into the account (2 MiB)', () => job(lord.fs.copy(`${home}/e2e-local-2m.bin`, `${T}/in.bin`, false)));
await step('in.bin goes up', () => seen(`${T}/in.bin`, 'here', 120000));
await step('copy out of the account', () => job(lord.fs.copy(`${T}/in.bin`, `${home}/e2e-out-__TAG__.bin`, false)));
await step('the copy out has every byte', () => lord.fs.stat(`${home}/e2e-out-__TAG__.bin`).size);
await step('list', () => lord.fs.list(T).map((e) => `${e.name}${e.dir ? '/' : ''}:${e.size}`));
out.T = T;
return out;
