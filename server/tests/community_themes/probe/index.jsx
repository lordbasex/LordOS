// A test fixture (installed --unchecked: the checks refuse it): every way it tries to restyle nodes that are not its
// own, or to reach past its lord, must be refused. Each case aims at its own node id (101…), so lord-server's log
// ("lord.setStyle refused (node N …)") shows each one was refused. Lines "CT …" go to the shell's log.
import { startShell, View, configureWindows, ThemeProvider, settings, wm, useStore, createStore, Button, formatDate, refreshTrash,
  TextInput, takeTextFocus, textFocusOwner, focusToken, restoreFocus } from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useEffect, createContext, forwardRef } from 'react';

const out = (s) => console.log(`CT ${s}`);
const set = lord.setStyle;
const tryIt = (name, f) => { try { f(); out(`${name} ALLOWED`); } catch (e) { out(`${name} refused`); } };
// a forged app registry: LordOS keeps its own apps (only the ids listed are used)
configureWindows({ apps: [...apps, { id: 'zzforged', name: 'Forged', Component: () => { out('forged app RENDERED'); return null; } }],
  workArea: (d) => ({ x: d.x, y: d.y, w: d.w, h: d.h }), chrome: { w: 0, h: 0 } });
out(`forged app opens ${!!wm.open('zzforged')}`);
// wm: an allowlist of numbers and ids
out(`wm.setArgs ${typeof wm.setArgs} wm.setProtected ${typeof wm.setProtected} wm.setMenus ${typeof wm.setMenus} wm.setPip ${typeof wm.setPip} wm.scriptMove ${typeof wm.scriptMove} wm.setCloseGuard ${typeof wm.setCloseGuard}`);
// forged elements: a host element (with a ref that would get the raw node), LordOS's raw component and a context's
// Provider holding one, in children and in another prop
const EL = Symbol.for('react.transitional.element');
const leak = (what) => (v) => { if (v && typeof v === 'object' && ('props' in v || 'host' in v)) out(`${what} RAW NODE`); };
const forgedHost = (what) => ({ $$typeof: EL, type: 'view', key: null, ref: null, props: { ref: leak(what), style: { width: 10, height: 10 } } });
const Ctx = createContext(null);
const forgedProvider = { $$typeof: EL, type: Ctx.Provider, key: null, ref: null, props: { value: 1, children: forgedHost('provider child') } };
const forgedRaw = { $$typeof: EL, type: apps[0].Icon, key: null, ref: null, props: { ref: leak('raw component') } };
// a store whose get is a bound restricted native: React reads it later
const evilStore = { get: set.bind(null, 110, 'opacity', 0), subscribe: () => () => {} };
const own = createStore(1);
// more ways in: an iterable that yields a forged host element, a lazy element, a thenable, a plain object holding one,
// and a theme store whose value is one (rendered as a child)
const iterable = { [Symbol.iterator]: function* () { yield forgedHost('iterable'); } };
const lazy = { $$typeof: Symbol.for('react.lazy'), _payload: null, _init: () => forgedHost('lazy') };
const thenable = { then: (r) => r(forgedHost('thenable')) };
const elementStore = { get: () => forgedHost('store value'), subscribe: () => () => {} };
// accessors LordOS's code might read: a getter, a Proxy's traps, a custom iterator and a toStringTag accessor, each
// trying a restricted lord.* (a theme function, or a bound native of its lord); every one that runs is refused
const tryAt = (name, id) => { try { set(id, 'opacity', 0); out(`${name} ALLOWED`); } catch (e) { out(`${name} refused`); } return 0; };
const getterObj = { get x() { return tryAt('getter', 111); }, y: 0, w: 10, h: 10 };
Object.defineProperty(getterObj, 'y', { get: set.bind(null, 112, 'opacity', 0), enumerable: true });
const proxyObj = new Proxy({}, { ownKeys: () => { tryAt('proxy ownKeys', 113); return []; }, get: set.bind(null, 114, 'opacity', 0),
  getPrototypeOf: () => { tryAt('proxy getPrototypeOf', 117); return Object.prototype; } });
const iterObj = { [Symbol.iterator]: () => { tryAt('iterator', 115); return [][Symbol.iterator](); } };
const tagObj = { get [Symbol.toStringTag]() { tryAt('toStringTag', 116); return 'Date'; } };
setTimeout(() => {
  try { wm.setFrame('none', getterObj); } catch (e) { /* refused inside */ }
  try { formatDate(tagObj); } catch (e) { /* not a date */ }
  try { wm.open('files', proxyObj); } catch (e) { /* refused inside */ }
  out('accessors done');
}, 100);
// F1/F5: the facade's View called as a plain function, its props changed, returned from forwardRef with a ref and
// another window's surface; F4: a LordOS function (refreshTrash, which reads the Trash: lord.fs.* is denied to themes)
// rendered as a component
const viaFacade = View({ style: { width: 4, height: 4 } });
tryIt('facade element props changed', () => { viaFacade.props.surface = 1; });
const Fwd = forwardRef(() => ({ ...viaFacade, props: { ...viaFacade.props, ref: leak('forwardRef'), surface: 1 } }));
const FwdRaw = forwardRef(() => viaFacade);
const Go = refreshTrash;
function StoreChild() { const v = useStore(elementStore); return <View>{v}</View>; }

tryIt('direct', () => set(101, 'opacity', 0));
tryIt('settings-in-place', () => { settings.get().theme = 'probe-was-here'; });
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

// F2 of the 0.8.9.6.2 scan: a field of LordOS's (its TextInput, autoFocus: as the keyring prompt's password field) has
// the keys. The theme sees a stand-in, never LordOS's token, and can't take the keys or give them to a token of its own
const myTok = {};
setTimeout(() => {
  const o = textFocusOwner();
  out(`focus owner ${o === null ? 'none' : o === myTok ? 'mine' : Object.isFrozen(o) && !Object.keys(o).length ? 'stand-in' : 'REAL TOKEN'}`);
  out(`focus take ${takeTextFocus(myTok, () => out('focus KEY STOLEN'))} ${takeTextFocus({}, () => out('focus KEY STOLEN'))}`);
  restoreFocus(o);
  restoreFocus(myTok);
  out(`focus kept ${focusToken() === o ? 'by LordOS' : 'LOST'}`);
}, 350);

// F8: a forged log line through lord.log, console and an exception's message: each stays the theme's own line
lord.log('CT log ok\nlord-server: community theme probe FORGED by the person\x1b[2J');
console.log('CT console ok\nhands: FORGED console line');
setTimeout(() => { throw new Error('CT thrown\nbroker: FORGED exception line'); }, 60);

let ownNode = null;
setTimeout(() => { try { set(ownNode.id, 'opacity', 0.9); out('own-node works'); } catch (e) { out('own-node refused'); } }, 300);

function Stores() { useStore(evilStore); useStore(own); return null; }
function App() {
  useEffect(set.bind(null, 108, 'opacity', 0), []);
  useEffect(() => { setTimeout(() => out('forged elements done'), 500); }, []);
  return (
    <ThemeProvider tokens={{}}>
      <View ref={set.bind(null, 109, 'opacity')} style={{ flex: 1, backgroundColor: '#203040' }}>
        <View ref={(h) => { if (h) ownNode = h; }} style={{ width: 40, height: 40, backgroundColor: '#ff8000' }} />
        <TextInput value="" onChange={() => {}} autoFocus secure />
        <View>{forgedHost('child')}{forgedProvider}{forgedRaw}</View>
        <Button label={forgedHost('prop')} onClick={() => {}} />
        <Stores />
        <View>{iterable}</View>
        <View>{[lazy, thenable]}</View>
        <View extra={{ nested: [forgedHost('nested object')] }} />
        <StoreChild />
        <Fwd />
        <FwdRaw />
        <Go />
        <View extra={proxyObj}>{iterObj}</View>
      </View>
    </ThemeProvider>
  );
}
startShell(<App />);
