// A test fixture (installed --unchecked: the checks refuse it): every way it tries to restyle nodes that are not its
// own, or to reach past its lord, must be refused. Each case aims at its own node id (101…), so lord-server's log
// ("lord.setStyle refused (node N …)") shows each one was refused. Lines "CT …" go to the shell's log.
import { startShell, View, configureWindows, ThemeProvider, settings } from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useEffect } from 'react';

const out = (s) => console.log(`CT ${s}`);
const set = lord.setStyle;
const tryIt = (name, f) => { try { f(); out(`${name} ALLOWED`); } catch (e) { out(`${name} refused`); } };
configureWindows({ apps, workArea: (d) => ({ x: d.x, y: d.y, w: d.w, h: d.h }), chrome: { w: 0, h: 0 } });

tryIt('direct', () => set(101, 'opacity', 0));
out(`lord.term ${typeof lord.term} lord.fs ${typeof lord.fs} lord.admin ${typeof lord.admin} globalThis.lord.perms ${typeof globalThis.lord.perms}`);
tryIt('term.create', () => lord.term.create());
tryIt('fs.write', () => lord.fs.write('/tmp/ct-probe', 'x'));
tryIt('own-realm-function', () => { const f = set.constructor('return lord'); if (f().fs) throw new Error('x'); throw new Error('no fs'); });
tryIt('host-function-constructor', () => View.constructor('return lord')());
tryIt('host-proto-change', () => { Object.getPrototypeOf(View).call2 = 1; });
Promise.resolve().then(set.bind(null, 102, 'opacity', 0)).then(() => out('promise ALLOWED'), () => out('promise refused'));
queueMicrotask(set.bind(null, 103, 'opacity', 0));
setTimeout(set.bind(null, 104, 'opacity', 0), 10);
setTimeout(set, 20, 105, 'opacity', 0);
requestAnimationFrame(set.bind(null, 106, 'opacity'));
requestAnimationFrame(() => out('raf ran'));
settings.subscribe(set.bind(null, 107, 'opacity', 0));
setTimeout(() => settings.set({ mode: settings.get().mode === 'light' ? 'dark' : 'light' }), 30);
setTimeout(() => out('timers ran'), 200);

let own = null;
setTimeout(() => tryIt('own-node', () => set(own.id, 'opacity', 0.9)), 300);

function App() {
  useEffect(set.bind(null, 108, 'opacity', 0), []);
  return (
    <ThemeProvider tokens={{}}>
      <View ref={set.bind(null, 109, 'opacity')} style={{ flex: 1, backgroundColor: '#203040' }}>
        <View ref={(h) => { if (h) own = h; }} style={{ width: 40, height: 40, backgroundColor: '#ff8000' }} />
      </View>
    </ThemeProvider>
  );
}
startShell(<App />);
