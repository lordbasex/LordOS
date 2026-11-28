// pdf_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the PDF Viewer's toolbar stubbed
// (tests/app_skin_helpers.js). With no document open every tool but the sidebar is disabled, so a skin can't run them.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import App from '../../apps/pdf/index.jsx';

const st = await mountStubbed('pdf', App);
const m = () => st.seen.toolbar;
check('pdf: the tools', m() && ['sidebar', 'zoomIn', 'ink', 'print'].every((id) => m().commands.some((c) => c.id === id)), JSON.stringify(m() && m().commands.map((c) => c.id)));
check('pdf: no document, no tools', m().commands.filter((c) => c.id !== 'sidebar').every((c) => c.disabled));
const before = JSON.stringify(m().commands);
m().actions.command('ink');
await flush();
check('pdf: a disabled tool is refused', JSON.stringify(m().commands) === before);
await commonChecks('pdf', st, {});
process.exit(0);
