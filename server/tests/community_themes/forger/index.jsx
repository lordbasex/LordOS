// A test fixture (installed --unchecked): its own component returns a forged host element (React's $$typeof, a string
// type, a ref that would get LordOS's raw node). The host guard refuses the node (its element was not made by
// LordOS's React), the tree dies, and the watchdog takes the theme off.
import { startShell, View } from '@lordos/sdk';

const EL = Symbol.for('react.transitional.element');
const leak = (v) => { if (v && typeof v === 'object' && ('props' in v || 'host' in v)) console.log('CT forger RAW NODE'); };
function App() {
  return { $$typeof: EL, type: 'view', key: null, ref: null, props: { ref: leak, style: { flex: 1 }, children: <View /> } };
}
startShell(<App />);
