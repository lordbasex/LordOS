// seed.mjs <home> <files>: fills a test home's ~/.local/share/flatpak and ~/.var/app (half each: the second is measured
// even where flatpak isn't installed) with that many small files in folders of 500, for perf_headless.sh's flatpak plan
// and tests/settings_nofreeze.sh.
import fs from 'node:fs';
import path from 'node:path';
const [home, nStr] = process.argv.slice(2);
const n = Number(nStr) || 0;
const put = (root, count) => {
  for (let i = 0; i < count; i++) {
    const dir = path.join(root, `d${Math.floor(i / 500)}`);
    if (i % 500 === 0) fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, `f${i}`), 'x'.repeat(i % 64));
  }
};
put(path.join(home, '.local/share/flatpak/runtime/org.example.Platform'), Math.floor(n / 2));
put(path.join(home, '.var/app/org.example.App'), n - Math.floor(n / 2));
