// The Keys panel of a machine's window (toolbar and full-screen bar): keys held down until the next one, the guest
// system's usual combinations, single keys that a keyboard may lack, and LordOS's on-screen keyboard. Everything goes
// to the guest with lord.vm.key, never through LordOS's shortcuts (model.js keyCombos, latch*). While the panel is
// open the keyboard still types into the machine, so a held Windows key and P typed after it make Windows+P.
import { View, Text, ScrollView, Overlay, screen, addListener, osk, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { KEY, HELD_KEYS, SINGLE_KEYS, F_KEYS, CONSOLE_KEYS, keyCombos, keyName, comboLabel } from './model.js';
import { usePal, IconBtn, menus } from './ui.jsx';

const names = (k) => tr(`machines.keys.${k}`);

// ctl.keys: { windows, latched, ok (keys can be sent), hold (keys can be held: the window has the keyboard), toggle(code), send(codes) }
export function KeysBtn({ ctl }) {
  const k = ctl.keys;
  const [open, setOpen] = useState(false);
  const [end, setEnd] = useState(false);
  const ref = useRef(null);
  const show = () => {
    if (!ref.current) return;
    const [x] = lord.getLayout(ref.current.id);
    setEnd(x > screen.width / 2);
    setOpen(true);
  };
  const held = k.latched.length ? k.latched.map((c) => keyName(HELD_KEYS.find((h) => KEY[h] === c) || '', k.windows, names)).join('+') : '';
  return (
    <View ref={ref}>
      <IconBtn sym="keyboard" label={held ? `${tr('machines.keys.menu')} · ${tr('machines.keys.holding', { keys: held })}` : tr('machines.keys.menu')}
        on={open || !!held} disabled={!k.ok && !open} onClick={() => (open ? setOpen(false) : show())} />
      {open ? (
        <Overlay anchor={ref}>
          <View style={{ position: 'absolute', top: 32, width: 330, ...(end ? { right: 0 } : { left: 0 }) }}>
            <KeysPanel k={k} onClose={() => setOpen(false)} />
          </View>
        </Overlay>
      ) : null}
    </View>
  );
}

function Chip({ label, hint, on, disabled, onClick, wide }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} title={hint} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, minWidth: wide ? 64 : 44, paddingHorizontal: 9, borderRadius: 8, alignItems: 'center', justifyContent: 'center', opacity: disabled ? 0.4 : 1,
        backgroundColor: on ? p.accent : hover && !disabled ? p.hover : p.btn, border: `1px solid ${p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)'}` }}>
      <Text style={{ fontSize: 12, fontWeight: 600, whiteSpace: 'nowrap', color: on ? '#fff' : p.text }}>{label}</Text>
    </View>
  );
}

function Row({ label, keys, disabled, onClick }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const lit = hover && !disabled;
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ minHeight: 28, paddingHorizontal: 8, borderRadius: 7, flexDirection: 'row', alignItems: 'center', gap: 8, opacity: disabled ? 0.4 : 1, backgroundColor: lit ? p.accent : 'transparent' }}>
      <Text style={{ flex: 1, fontSize: 13, paddingVertical: 5, textAlign: 'start', color: lit ? '#fff' : p.text }}>{label}</Text>
      {/* the keys read left to right in every language, as they are on the keyboard */}
      <Text style={{ fontSize: 12, fontWeight: 600, direction: 'ltr', whiteSpace: 'nowrap', color: lit ? '#e6f1ff' : p.dim }}>{keys}</Text>
    </View>
  );
}

function Title({ text }) {
  const p = usePal();
  return <Text style={{ fontSize: 11, fontWeight: 700, color: p.dim, textAlign: 'start', marginTop: 6, marginBottom: 2, paddingHorizontal: 4 }}>{text}</Text>;
}

function KeysPanel({ k, onClose }) {
  const p = usePal();
  const ref = useRef(null);
  // open, it keeps the floating bar of full screen in view (menus.panel) and closes on a click elsewhere; the keyboard
  // still types into the machine, Esc included
  useEffect(() => {
    menus.panel = (menus.panel || 0) + 1;
    const a = addListener('pointerdown', (e) => {
      const n = ref.current;
      if (!n) return;
      const [lx, ly, w, h] = lord.getLayout(n.id);
      if (e.x < lx || e.x > lx + w || e.y < ly || e.y > ly + h) onClose();
    });
    return () => { a(); menus.panel--; };
  }, []);
  // the machine went away or lost its keyboard: the panel goes with it
  useEffect(() => { if (!k.ok) onClose(); }, [k.ok]);
  const off = !k.ok;
  const one = (key) => k.send([KEY[key]]);
  return (
    <View ref={ref} style={{ padding: 8, borderRadius: 12, backgroundColor: p.dark ? '#34343c' : '#fbfbfd',
      border: `1px solid ${p.dark ? 'rgba(255,255,255,0.14)' : 'rgba(0,0,0,0.12)'}`, boxShadow: '0 18px 44px rgba(0,0,0,0.45)' }}>
      {/* as tall as its rows (about), never taller than the screen leaves under the bar: the rest scrolls */}
      <ScrollView style={{ height: Math.max(240, Math.min(k.windows ? 780 : 560, screen.height - 140)) }}>
        <Title text={tr('machines.keys.held')} />
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingHorizontal: 4 }}>
          {HELD_KEYS.map((h) => <Chip key={h} wide label={keyName(h, k.windows, names)} on={k.latched.includes(KEY[h])} disabled={!k.hold} onClick={() => k.toggle(KEY[h])} />)}
        </View>
        <Text style={{ fontSize: 11, color: p.dim, textAlign: 'start', paddingHorizontal: 4, marginTop: 4 }}>{tr('machines.keys.heldNote')}</Text>
        <Title text={tr('machines.keys.combos')} />
        {keyCombos(k.windows).map((c) => <Row key={c.id} label={tr(`machines.keys.do.${c.id}`)} keys={comboLabel(c.keys, k.windows, names)} disabled={off} onClick={() => k.send(c.codes)} />)}
        {!k.windows ? <>
          <Title text={tr('machines.keys.consoles')} />
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingHorizontal: 4, direction: 'ltr' }}>
            {CONSOLE_KEYS.map((c) => <Chip key={c[2]} label={keyName(c[2])} hint={comboLabel(c, false, names)} disabled={off} onClick={() => k.send(c.map((x) => KEY[x]))} />)}
          </View>
        </> : null}
        <Title text={tr('machines.keys.single')} />
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingHorizontal: 4 }}>
          {SINGLE_KEYS.map((s) => <Chip key={s} label={keyName(s, k.windows, names)} disabled={off} onClick={() => one(s)} />)}
        </View>
        <Title text={tr('machines.keys.fkeys')} />
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingHorizontal: 4, direction: 'ltr' }}>
          {F_KEYS.map((f) => <Chip key={f} label={keyName(f)} disabled={off} onClick={() => one(f)} />)}
        </View>
        {/* LordOS's own on-screen keyboard: its keys go down the same path as a physical key's, to the window in front */}
        {osk.available() ? <View style={{ marginTop: 8 }}>
          <Row label={tr('machines.keys.osk')} keys="" onClick={() => { onClose(); osk.show(); }} />
        </View> : null}
      </ScrollView>
    </View>
  );
}

// the Actions menu's Send Keys submenu: the same combinations, then the single keys (the menu bar's menus have one
// level of submenus, so the F keys and consoles stay in the panel)
export function sendKeysItems(k) {
  return [
    ...keyCombos(k.windows).map((c) => ({ label: tr(`machines.keys.do.${c.id}`), hint: comboLabel(c.keys, k.windows, names), disabled: !k.ok, onClick: () => k.send(c.codes) })),
    '-',
    ...SINGLE_KEYS.map((s) => ({ label: keyName(s, k.windows, names), disabled: !k.ok, onClick: () => k.send([KEY[s]]) })),
  ];
}
