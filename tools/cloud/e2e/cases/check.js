// check.js — Check This Account (docs/CLOUD.md 19.3) on account __NAME__, with the env Settings gives it
const { runCheck, buildReport, aboutFields } = globalThis.__e2eCheck;
await until(() => A('__NAME__') && A('__NAME__').mounted, 30000);
const a = A('__NAME__');
const fs = {
  home: () => lord.fs.home(), list: (p) => lord.fs.list(p), mkdir: (p) => lord.fs.mkdir(p), write: (p, d) => lord.fs.write(p, d),
  read: (p, m) => lord.fs.read(p, m), stat: (p) => lord.fs.stat(p), move: (x, y) => lord.fs.copy(x, y, true), jobs: () => lord.fs.jobs(),
  forgetJob: (id) => lord.fs.jobForget(id), trash: (p) => lord.fs.trash(p), trashList: () => lord.fs.trashList(), removeFromTrash: (id) => lord.fs.removeFromTrash(id),
};
const steps = await runCheck(a, { fs, cloud, sleep, now: () => Date.now() });
const acc = A(a.id);
const report = buildReport({ version: '0.8.8.5-e2e', arch: 'x86_64', rclone: S().service.version, kind: a.kind, label: a.provider, verified: true,
  ownApp: !!acc.ownClient, aboutFields: aboutFields(acc.about), linksSupported: !!acc.linksSupported, steps });
return { steps: steps.map((s) => `${s.result} ${s.id} ${Math.round(s.ms)}ms ${s.note || ''}`), report };
