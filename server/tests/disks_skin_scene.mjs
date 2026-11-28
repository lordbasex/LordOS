// disks_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Disks with its list of disks stubbed
// (tests/app_skin_helpers.js). select() opens only a listed disk or partition; the actions stay Disks'.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Disks from '../../apps/disks/index.jsx';

const DISKS = [
  { name: 'sda', model: 'Demo SSD', size: 5e11, removable: false, system: true, transport: 'sata', table: 'gpt', partitions: [
    { name: 'sda1', number: 1, label: 'EFI', size: 5e8, fstype: 'vfat' }, { name: 'sda2', number: 2, label: 'root', size: 4.9e11, fstype: 'ext4' }] },
  { name: 'sdb', model: 'Demo USB', size: 3.2e10, removable: true, transport: 'usb', table: 'dos', partitions: [{ name: 'sdb1', number: 1, label: 'STICK', size: 3.2e10, fstype: 'exfat' }] },
];
globalThis.lord.disks = new Proxy({ list: () => DISKS, tools: () => ({}), watch: () => true }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
const st = await mountStubbed('disks', Disks);
const m = () => st.seen.sidebar;
check('disks: the disks and partitions', m() && m().items.map((x) => x.key).join() === 'd:sda,p:sda:sda1,p:sda:sda2,d:sdb,p:sdb:sdb1', JSON.stringify(m() && m().items.map((x) => x.key)));
check('disks: the removable one is selected first', m().items.find((x) => x.key === 'd:sdb').selected);
m().actions.select('p:sda:sda2');
await flush();
check('disks: a partition', m().items.find((x) => x.key === 'p:sda:sda2').selected);
m().actions.select('p:sdz:sdz1');
await flush();
check('disks: a disk not listed is refused', m().items.find((x) => x.key === 'p:sda:sda2').selected);
await commonChecks('disks', st, { bar: 'sidebar' });
process.exit(0);
