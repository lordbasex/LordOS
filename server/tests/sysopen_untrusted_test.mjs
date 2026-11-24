// sysopen_untrusted_test.mjs — openFileWithApps({ untrusted }) never hands a file that would run (a launcher, a program,
// a script, Java, Wine, a package) to a Linux app: it is shown in Files instead; documents still open.
import { runsCode } from '../../sdk/runscode.js';
let fail = 0, n = 0;
const ok = (c, m) => { n++; if (!c) { fail++; console.log('FAIL ' + m); } };
for (const f of ['a.desktop', 'X.AppImage', 'setup.exe', 'i.msi', 'tool.jar', 'r.sh', 'p.py', 'k.deb', 'x.flatpakref', 'w.bat', 'lib.so', 'a.run', 'app.lpk'])
  ok(runsCode(f), 'runs code: ' + f);
for (const f of ['report.pdf', 'photo.jpg', 'notes.txt', 'sheet.ods', 'doc.docx', 'song.mp3', 'archive.zip', 'page.html', ''])
  ok(!runsCode(f), 'does not run code: ' + f);
globalThis.lord = { mime: { guess: (x) => (x === 'noext' ? 'application/x-executable' : x === 'odd' ? 'application/x-java-archive' : 'application/octet-stream') } };
ok(runsCode('noext'), 'an ELF without extension (by its type)');
ok(runsCode('odd'), 'a Java archive without extension (by its type)');
ok(!runsCode('blob'), 'an unknown binary is not called a program');
console.log(`sysopen-untrusted-test: ${n - fail} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
