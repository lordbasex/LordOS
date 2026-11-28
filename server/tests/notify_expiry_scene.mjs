// notify_expiry_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): a banner whose time is up
// (sdk/notify.js) goes with the reason 'expired', never 'closed', so a Linux or Flatpak app's notification stays in the
// centre with its buttons (sdk/hardware.jsx tells the app nothing then); the history keeps the icon it came with, for a
// centre whose app the registry doesn't list (a Flatpak app's). QA of 0.8.9.6.2. Prints "ok <name>" or "FAIL <name>".
import './sentinela_scene_stubs.js';
import { notify, notifications, notifyHistory, dismissNotification } from '../../sdk/notify.js';

const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const Icon = () => null;
const reasons = [];
let ran = 0;
const id = notify({ app: 'linux:org.gnome.clocks', label: 'Clocks', icon: Icon, title: 'Alarm', timeout: 1500,
  actions: [{ id: '0', label: 'Stop', run: () => { ran++; } }], onClose: (why) => reasons.push(why), sound: false });
check('notify-expiry: shown', id > 0 && notifications.get().some((n) => n.id === id));
await wait(1700);
check('notify-expiry: its time up, the banner goes as expired', !notifications.get().some((n) => n.id === id) && reasons.join() === 'expired', reasons.join());
const h = notifyHistory.get().find((n) => n.id === id);
check('notify-expiry: the centre keeps it, its icon and its button', !!h && h.Icon === Icon && h.actions.length === 1);
h.actions[0].run();
check('notify-expiry: its button still runs', ran === 1);
const id2 = notify({ app: 'x', title: 'y', onClose: (why) => reasons.push(why), sound: false });
dismissNotification(id2);
check('notify-expiry: a banner the person closes is closed', reasons[reasons.length - 1] === 'closed', reasons.join());
process.exit(0);
