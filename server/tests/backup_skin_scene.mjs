// backup_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Backups with its two tabs stubbed
// (tests/app_skin_helpers.js); the backups, restoring and the settings stay the app's.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import App from '../../apps/backup/index.jsx';

const st = await mountStubbed('backup', App);
const m = () => st.seen.toolbar;
check('backup: two tabs', m() && m().commands.map((c) => c.id).join() === 'snapshots,settings');
m().actions.command('snapshots');
await flush();
check('backup: a tab', m().commands[0].active);
await commonChecks('backup', st, {});
process.exit(0);
