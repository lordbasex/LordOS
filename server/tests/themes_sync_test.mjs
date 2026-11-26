// themes_sync_test.mjs — tools/themes-sync.mjs: the vendored official themes (themes/<id>/) are exactly the pinned
// LordOS-themes commit's (themes/upstream.json, themes/catalog.json), and a catalog that names a path outside its
// folder, a reserved id or a theme twice is refused before anything is read from the clone.
import { checkCatalog, checkTree } from '../../tools/themes-sync.mjs';

let ok = 0, bad = 0;
const eq = (what, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${what}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const tree = checkTree();
for (const p of tree) console.log(`  ${p}`);
eq('the tree is the pinned snapshot', tree.length, 0);

const H = 'a'.repeat(64);
const f = (path) => ({ path, size: 1, sha256: H });
const theme = (id, folder, files = [f('index.jsx'), f('theme.json')]) => ({ id, folder, files, docs: [f('README.md')], screenshots: [f('screenshots/desktop.png')] });
const cat = (...themes) => ({ format: 1, themes });

eq('a good catalog', checkCatalog(cat(theme('default', 'flow'), theme('luna', 'luna'))), []);
eq('format', checkCatalog({ format: 2, themes: [] }).length > 0, true);
eq('dot-dot', checkCatalog(cat(theme('x', 'x', [f('index.jsx'), f('theme.json'), f('../../etc/passwd')]))).length > 0, true);
eq('absolute', checkCatalog(cat(theme('x', 'x', [f('index.jsx'), f('theme.json'), f('/etc/passwd')]))).length > 0, true);
eq('hidden', checkCatalog(cat(theme('x', 'x', [f('index.jsx'), f('theme.json'), f('a/.git/config')]))).length > 0, true);
eq('bad folder', checkCatalog(cat(theme('x', '../x'))).length > 0, true);
eq('greeter reserved', checkCatalog(cat(theme('greeter', 'greeter'))).length > 0, true);
eq('twice', checkCatalog(cat(theme('a', 'a'), theme('a', 'b'))).length > 0, true);
eq('same folder', checkCatalog(cat(theme('a', 'x'), theme('b', 'x'))).length > 0, true);
eq('needs index.jsx', checkCatalog(cat(theme('a', 'a', [f('theme.json')]))).length > 0, true);
eq('bad hash', checkCatalog(cat(theme('a', 'a', [f('index.jsx'), { path: 'theme.json', size: 1, sha256: 'xyz' }]))).length > 0, true);

console.log(`themes-sync-test: ${ok} ok${bad ? `, ${bad} failed` : ''}`);
process.exit(bad ? 1 : 0);
