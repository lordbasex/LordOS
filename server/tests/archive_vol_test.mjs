// archive_vol_test.mjs — split archives in Files (sdk/archivevol.js, GitHub #103): which names are volumes,
// the reading order, a missing volume, and plain archives left alone.
//
//   node tests/archive_vol_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { splitVolume, volumeSet, setName, MAX_VOLUMES } = await import(path.join(here, '../../sdk/archivevol.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const folder = (...names) => { const s = new Set(names.map((x) => `/h/d/${x}`)); return (p) => s.has(p); };

// ---- names ----
eq('zip.001', splitVolume('DOCUMENTS.zip.001'), { base: 'DOCUMENTS.zip', kind: 'num', index: 1, digits: 3 });
eq('7z.014', splitVolume('a.7z.014')?.index, 14);
eq('tar.gz.002', splitVolume('b.tar.gz.002')?.base, 'b.tar.gz');
eq('z01', splitVolume('x.z01'), { base: 'x', kind: 'z', index: 1 });
eq('Z12 upper case', splitVolume('X.Z12')?.index, 12);
eq('zip as last volume', splitVolume('x.zip'), { base: 'x', kind: 'z', index: 0 });
eq('.000 is not a volume', splitVolume('a.zip.000'), null);
eq('plain .001 file', splitVolume('notes.txt.001'), null);
eq('mp3', splitVolume('song.mp3'), null);
eq('setName num', setName('DOCUMENTS.zip.003'), 'DOCUMENTS.zip');
eq('setName z', setName('x.z02'), 'x.zip');
eq('setName plain', setName('photo.jpg'), 'photo.jpg');

// ---- byte splits ----
const v14 = Array.from({ length: 14 }, (_, i) => `DOCUMENTS.zip.${String(i + 1).padStart(3, '0')}`);
eq('14 volumes from the first', volumeSet('/h/d/DOCUMENTS.zip.001', folder(...v14, 'other.txt')), { paths: v14.map((x) => `/h/d/${x}`), missing: null });
eq('the same set from a middle volume', volumeSet('/h/d/DOCUMENTS.zip.007', folder(...v14)).paths.length, 14);
eq('a lone .001 is a whole archive', volumeSet('/h/d/a.7z.001', folder('a.7z.001')), { paths: ['/h/d/a.7z.001'], missing: null });
eq('hole in the middle', volumeSet('/h/d/a.7z.001', folder('a.7z.001', 'a.7z.002', 'a.7z.004')).missing, 'a.7z.003');
eq('first one missing', volumeSet('/h/d/a.7z.002', folder('a.7z.002', 'a.7z.003')), { paths: [], missing: 'a.7z.001' });
eq('opened volume past the end of what is there', volumeSet('/h/d/a.7z.009', folder('a.7z.001', 'a.7z.009')).missing, 'a.7z.002');
eq('a plain zip is not a set', volumeSet('/h/d/a.zip', folder('a.zip')), null);
eq('a relative path', volumeSet('a.zip.001', (p) => p === 'a.zip.001' || p === 'a.zip.002').paths, ['a.zip.001', 'a.zip.002']);
const many = Array.from({ length: 300 }, (_, i) => `m.zip.${String(i + 1).padStart(3, '0')}`);
eq('capped at the helper limit', volumeSet('/h/d/m.zip.001', folder(...many)).paths.length, MAX_VOLUMES);

// ---- Info-ZIP splits: .z01 … .zip last ----
eq('z01 set', volumeSet('/h/d/x.z01', folder('x.z01', 'x.z02', 'x.zip')), { paths: ['/h/d/x.z01', '/h/d/x.z02', '/h/d/x.zip'], missing: null });
eq('opened from the .zip', volumeSet('/h/d/x.zip', folder('x.z01', 'x.z02', 'x.zip')).paths, ['/h/d/x.z01', '/h/d/x.z02', '/h/d/x.zip']);
eq('the .zip is missing', volumeSet('/h/d/x.z01', folder('x.z01', 'x.z02')).missing, 'x.zip');
eq('a .z hole', volumeSet('/h/d/x.zip', folder('x.z01', 'x.z03', 'x.zip')).missing, 'x.z02');
eq('z01 missing, z02 opened', volumeSet('/h/d/x.z02', folder('x.z02', 'x.zip')).missing, 'x.z01');

console.log(`archive-vol: ${n - bad}/${n}`);
process.exit(bad ? 1 : 0);
