// terminal_fwdports_test.mjs — port forwards whose fixed local port is taken (apps/terminal/fwdports.js).
import { fwdKey, applyFree, takenPorts, badgeItem, badgeText } from '../../apps/terminal/fwdports.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const web = { host: 'localhost', port: 80, want: 8080 }, db = { host: 'db', port: 5432, want: 0 }, api = { host: 'localhost', port: 3000, want: 3000 };
eq('key', fwdKey(web), 'localhost:80:8080');
eq('nothing moved: the profile as is', applyFree([web, db], new Set()), [web, db]);
eq('no set yet', applyFree([web], undefined), [web]);
eq('moved to a free port for this session', applyFree([web, db, api], new Set(['localhost:80:8080'])), [{ ...web, want: 0 }, db, api]);
eq('another fixed port keeps its own', applyFree([{ ...web, want: 8081 }], new Set(['localhost:80:8080'])), [{ ...web, want: 8081 }]);

const items = [
  { ...web, state: 'error', error: 'inuse', local: 0 },
  { ...api, state: 'error', error: 'inuse', local: 0 },
  { ...db, state: 'open', error: '', local: 54321 },
  { host: 'x', port: 81, want: 8080, state: 'error', error: 'inuse', local: 0 },
  { host: 'y', port: 82, want: 9000, state: 'error', error: 'failed', local: 0 },
];
eq('taken ports, once each, in order', takenPorts(items, new Set()), [8080, 3000]);
eq('already told', takenPorts(items, new Set([8080])), [3000]);
eq('none taken', takenPorts([items[2], items[4]], new Set()), []);
eq('no list', takenPorts(undefined, undefined), []);

eq('badge: an open one first', badgeItem(items).port, 5432);
eq('badge: a taken port before other errors', badgeItem([items[4], items[1]]).port, 3000);
eq('badge: else the first', badgeItem([items[4]]).port, 82);
eq('badge text: open', badgeText([items[2]]), '⇄ 5432→54321');
eq('badge text: taken, with the others', badgeText([items[0], items[4]]), '⇄ 80→8080 ! +1');
eq('badge text: waiting', badgeText([{ ...db, state: 'wait', local: 0 }]), '⇄ 5432→…');

console.log(`terminal_fwdports_test: ${ok}/${ok + bad} checks`);
process.exit(bad ? 1 : 0);
