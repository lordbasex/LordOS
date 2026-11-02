// NetTree.jsx — Monitor → Network's left column, second generation (0.8.5, docs/MONITOR.md):
//   TokenSearch   the search field: "app:firefox", "port:443"… (and, on Enter, any word) become chips
//                 that combine (sdk/netmodel.js parseSearch / chipFor)
//   ViewButton    one pop-up button with the whole view: group by (app, domain, IP, port, country),
//                 direction, protocol, status and sort; remembered per user (settings.monitor.netView)
//   NetTree       the tree: app → destination (name or address) → address → port, or the other
//                 groupings, each row with its sent and received bytes, a small up/down meter and,
//                 where a rule can be written, its switch; "N more…" folds long lists
import {
  View, Text, SearchField, Overlay, useTheme, useLocale, useSettings, formatBytes, formatNumber, tOptional, direction, firewall, fwRule, netModel, addListener,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { C, AppGlyph } from './index.jsx';

const { parseSearch, addChips, fold, normView } = netModel;
const countryName = (cc) => (cc ? tOptional(`countries.${cc}`) || cc : '');

// ---- search with chips ----

const CHIP_COLORS = { app: '#0a84ff', domain: '#30b0c7', ip: '#5e5ce6', port: '#ff9f0a', proto: '#bf5af2', country: '#34c759' };

export function TokenSearch({ chips, setChips, text, setText, tr }) {
  const t = useTheme();
  // a finished "type:value" token (followed by a space) becomes a chip at once; Enter turns every word
  const change = (v) => {
    if (/\s$/.test(v)) {
      const q = parseSearch(v, false);
      if (q.chips.length) { setChips(addChips(chips, q.chips)); setText(q.rest ? `${q.rest} ` : ''); return; }
    }
    setText(v);
  };
  const commit = () => {
    const q = parseSearch(text, true);
    if (q.chips.length) setChips(addChips(chips, q.chips));
    setText(q.rest);
  };
  return (
    <View style={{ gap: 6 }}>
      <SearchField value={text} onChange={change} onSubmit={commit} placeholder={tr('monitor.net.searchTokens')}
        onKeyDown={(e) => { if ((e.key === 'Backspace') && !text && chips.length) { setChips(chips.slice(0, -1)); return true; } return false; }} />
      {chips.length ? (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
          {chips.map((c) => (
            <View key={`${c.type}:${c.value}`} onClick={() => setChips(chips.filter((x) => x !== c))}
              style={{ flexDirection: 'row', alignItems: 'center', gap: 4, height: 20, paddingHorizontal: 7, borderRadius: 10, backgroundColor: `${CHIP_COLORS[c.type]}2e` }}>
              <Text style={{ fontSize: 10, fontWeight: 600, color: CHIP_COLORS[c.type] }}>{tr(`monitor.net.chip.${c.type}`)}</Text>
              <Text style={{ fontSize: 11, color: t.text, direction: 'ltr' }}>{c.type === 'country' && c.value.length === 2 ? countryName(c.value) : c.value}</Text>
              <Text style={{ fontSize: 10, color: t.textDim }}>✕</Text>
            </View>
          ))}
          <View onClick={() => setChips([])} style={{ height: 20, justifyContent: 'center', paddingHorizontal: 4 }}>
            <Text style={{ fontSize: 10, color: t.accent }}>{tr('monitor.net.clearChips')}</Text>
          </View>
        </View>
      ) : null}
    </View>
  );
}

// ---- the view pop-up button ----

export function useNetView() {
  const [s, set] = useSettings();
  const m = s.monitor || {};
  const view = normView(m.netView);
  const setView = (patch) => set({ monitor: { ...m, netView: { ...view, ...patch } } });
  return [view, setView, m, (patch) => set({ monitor: { ...m, ...patch } })];
}

const SECTIONS = [
  ['group', ['app', 'domain', 'ip', 'port', 'country']],
  ['dir', ['all', 'out', 'in']],
  ['proto', ['all', 'tcp', 'udp']],
  ['status', ['all', 'allowed', 'denied', 'rules']],
  ['sort', ['name', 'most', 'least', 'recent', 'conns']],
];

export function ViewButton({ view, setView, extra, tr }) {
  const t = useTheme();
  const [open, setOpen] = useState(false);
  const btn = useRef(null), menu = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, h] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + h; };
      if (!inside(menu.current) && !inside(btn.current)) setOpen(false);
    });
  }, [open]);
  const changed = ['dir', 'proto', 'status'].filter((k) => view[k] !== 'all').length;
  return (
    <View ref={btn}>
      <View onClick={() => setOpen((o) => !o)} style={{ height: 24, paddingHorizontal: 9, borderRadius: t.controlRadiusSmall || 6, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: t.control, boxShadow: '0 1px 1px rgba(0,0,0,0.12)', ...(t.buttonBorder ? { border: `1px solid ${t.separator}` } : null) }}>
        <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{tr('monitor.net.view.button', { group: tr(`monitor.net.view.group.${view.group}`) })}</Text>
        {changed ? (
          <View style={{ minWidth: 14, height: 14, borderRadius: 7, paddingHorizontal: 3, backgroundColor: t.accent, alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 9, fontWeight: 700, color: '#fff' }}>{formatNumber(changed, 0)}</Text>
          </View>
        ) : null}
        {t.menuHighlight === 'subtle'
          ? <View style={{ width: 16, height: 18, alignItems: 'center', justifyContent: 'center' }}>
            <View style={{ width: 6, height: 6, marginTop: -3, borderRightWidth: 1.5, borderBottomWidth: 1.5, borderColor: t.textDim, transform: 'rotate(45deg)' }} />
          </View>
          : <View style={{ width: 16, height: 18, borderRadius: 4, backgroundColor: t.accent, alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 8, color: '#fff', lineHeight: 8 }}>▲</Text>
            <Text style={{ fontSize: 8, color: '#fff', lineHeight: 8 }}>▼</Text>
          </View>}
      </View>
      {open && (
        <Overlay anchor={btn}>
          <View ref={menu} style={{ position: 'absolute', top: 28, [direction() === 'rtl' ? 'right' : 'left']: 0, width: 420, paddingVertical: 6, paddingHorizontal: 5,
            borderRadius: t.menuRadius, backgroundColor: t.popupBg || (t.mode === 'dark' ? '#2c2c31' : '#ffffff'), border: `1px solid ${t.separator}`,
            boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            {/* two columns: what the list is (grouping, order) and what it shows (direction, protocol, status) */}
            <View style={{ flexDirection: 'row', gap: 4 }}>
              {[['group', 'sort'], ['dir', 'proto', 'status']].map((col, ci) => (
                <View key={ci} style={{ flex: 1, minWidth: 0 }}>
                  {col.map((key, si) => (
                    <View key={key}>
                      {si ? <View style={{ height: 1, marginVertical: 3, marginHorizontal: 6, backgroundColor: t.separator }} /> : null}
                      <Text style={{ fontSize: 10, fontWeight: 600, color: t.textDim, paddingHorizontal: 8, paddingVertical: 2 }}>{tr(`monitor.net.view.${key}`)}</Text>
                      {SECTIONS.find(([k]) => k === key)[1].map((v) => <MenuItem key={v} label={tr(`monitor.net.view.${key}.${v}`)} checked={view[key] === v} onClick={() => setView({ [key]: v })} />)}
                    </View>
                  ))}
                </View>
              ))}
            </View>
            {(extra || []).length ? <View style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: t.separator }} /> : null}
            {(extra || []).map((it) => (
              <MenuItem key={it.label} label={it.label} checked={it.checked} onClick={() => { setOpen(false); it.onClick(); }} />
            ))}
          </View>
        </Overlay>
      )}
    </View>
  );
}

function MenuItem({ label, checked, onClick }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const on = hover && t.menuHighlight !== 'subtle';
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 21, paddingHorizontal: 8, borderRadius: t.menuItemRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: on ? t.accent : hover ? t.hover : 'transparent' }}>
      <Text style={{ width: 12, fontSize: 11, color: on ? '#fff' : t.text }}>{checked ? '✓' : ''}</Text>
      <Text style={{ flex: 1, fontSize: 12, whiteSpace: 'nowrap', color: on ? '#fff' : t.text, textAlign: 'start' }}>{label}</Text>
    </View>
  );
}

// ---- the tree ----

// A small up/down meter: received (down, green) and sent (up, orange) on a log scale of the rate.
function Meter({ rx, tx, selected }) {
  const t = useTheme();
  const h = (v) => (v > 0 ? Math.max(1, Math.min(6, Math.round(Math.log10(v / 64 + 1) * 1.6))) : 0);
  return (
    <View style={{ width: 9, height: 14, justifyContent: 'center', alignItems: 'center', direction: 'ltr' }}>
      <View style={{ width: 4, height: 6, justifyContent: 'flex-end' }}>
        <View style={{ width: 4, height: h(tx), borderRadius: 1, backgroundColor: selected ? '#fff' : C.tx }} />
      </View>
      <View style={{ width: 7, height: 1, backgroundColor: selected ? 'rgba(255,255,255,0.5)' : t.separator }} />
      <View style={{ width: 4, height: 6 }}>
        <View style={{ width: 4, height: h(rx), borderRadius: 1, backgroundColor: selected ? '#fff' : C.rx }} />
      </View>
    </View>
  );
}

// "↓ 1.2 MB ↑ 40 KB", shorter when one side is empty
export function upDownShort(n) {
  const parts = [];
  if (n.rx) parts.push(`↓ ${formatBytes(n.rx)}`);
  if (n.tx) parts.push(`↑ ${formatBytes(n.tx)}`);
  return parts.join(' ') || '—';
}

const placeOf = (tr, n) => (n.cc ? countryName(n.cc) : n.scope ? tr(`monitor.net.scope.${n.scope}`) : '');

// The verdict of a destination row for its app (sdk/fwrule.js decide, as the kernel decides), and a
// click that writes, flips or removes the rule for exactly that name or address.
function DestSwitch({ app, name, addr, port, proto, listed, mode, restricted }) {
  if (!firewall.available() || !firewall.enforceable(app)) return null;
  const rules = firewall.rules(app);
  const opts = { listed: !!listed, mode };
  const q = { names: name ? [name] : [], addr, port: port || 443, proto: proto === 'udp' ? 'udp' : 'tcp' };
  const v = fwRule.decide(rules, q, restricted, opts);
  const on = v ? v.action === 'allow' : true;
  const locked = v?.by === 'restricted';
  const flip = (e) => {
    e.stopPropagation();
    if (locked) return;
    const op = fwRule.toggleOp(rules, { name, addr, proto: q.proto, port: q.port }, restricted, opts);
    if (!op) return;
    if (op.op === 'add') firewall.addRule({ app, ...op.rule });
    else if (op.op === 'update') firewall.updateRule(op.id, op.rule);
    else firewall.removeRule(op.id);
  };
  return (
    <View onClick={flip} style={{ width: 22, height: 12, borderRadius: 6, backgroundColor: on ? 'rgba(48,209,88,0.32)' : 'rgba(255,69,58,0.32)',
      opacity: locked ? 0.45 : 1, justifyContent: 'center', alignItems: on ? 'flex-end' : 'flex-start', paddingHorizontal: 2, direction: 'ltr' }}>
      <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: on ? '#30d158' : '#ff453a' }} />
    </View>
  );
}

const SHOWN = 8;

// Why a destination is allowed or blocked when its own switch isn't what decides: a blocklist, a
// restriction, another rule ("*.example.com") or the app's default.
function verdictHint(tr, ctx, app, name, addr, port, proto, listed) {
  if (!app || !firewall.available() || !firewall.enforceable(app)) return listed ? tr('monitor.net.onList', { list: ctx.listName(listed) }) : '';
  const v = fwRule.decide(firewall.rules(app), { names: name ? [name] : [], addr, port: port || 443, proto: proto === 'udp' ? 'udp' : 'tcp' },
    ctx.restrictedOf(app), { listed: !!listed, mode: ctx.mode });
  if (!v) return '';
  if (v.by === 'list') return tr('monitor.net.blockedByList', { list: ctx.listName(listed) });
  if (v.by === 'restricted') return tr('monitor.net.domainRestricted');
  if (v.by === 'mode') return tr('monitor.net.deniedByMode');
  const own = name ? fwRule.normDomain(name) : addr;
  if (v.rule.dst === own && !v.rule.port && !v.rule.proto) return '';
  return v.rule.dst === 'any' ? tr('monitor.net.domainByDefault') : tr('monitor.net.domainByRule', { rule: v.rule.dst });
}

// One row below the top level (a destination, an address, a port or an app under a destination).
export function Child({ node, depth, path, ctx }) {
  const t = useTheme();
  const { tr } = ctx;
  const key = path ? `${path}\u0001${node.key}` : node.key;
  const open = ctx.open.has(key);
  const kids = node.children || [];
  const { list, more } = fold(kids, SHOWN, ctx.all.has(key));
  const appOf = ctx.appOf(node, path);
  const sel = ctx.selectedHost && node.addr && ctx.selectedHost === node.addr && ctx.selected === appOf;
  const label = node.kind === 'app' ? ctx.appLabel(node) : node.kind === 'country' ? (node.cc ? countryName(node.cc) : tr(`monitor.net.scope.${node.scope || 'local'}`)) : node.label;
  const dot = node.denied || node.blocked ? '#ff453a' : node.listed ? '#ff9f0a' : node.rateRx + node.rateTx > 0 ? t.accent : t.textDim;
  const destName = node.kind === 'domain' ? node.name : null;
  const destAddr = node.addr || node.flows[0]?.addr;
  const canSwitch = appOf && (node.kind === 'domain' || node.kind === 'ip') && !ctx.isSystem(appOf);
  const inset = depth ? 14 + depth * 14 : 4;
  return (
    <View>
      <View onClick={() => ctx.onPick(node, appOf)} style={{ flexDirection: 'row', alignItems: 'center', gap: 5, minHeight: depth ? 22 : 32, borderRadius: 5,
        ...(direction() === 'rtl' ? { paddingRight: inset } : { paddingLeft: inset }), backgroundColor: sel ? (t.mode === 'dark' ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.05)') : 'transparent' }}>
        <View onClick={(e) => { if (!kids.length) return; e.stopPropagation(); ctx.toggle(key); }} style={{ width: 10, alignItems: 'center' }}>
          <Text style={{ fontSize: 8, color: t.textDim }}>{kids.length ? (open ? '▾' : direction() === 'rtl' ? '◂' : '▸') : ''}</Text>
        </View>
        {node.kind === 'app' ? <AppGlyph item={ctx.itemOf(node)} size={13} /> : <View style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: dot }} />}
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: depth ? 11 : 12, fontWeight: depth ? 400 : 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', fontFamily: node.kind === 'app' || node.kind === 'country' ? undefined : 'monospace',
            direction: node.kind === 'app' || node.kind === 'country' ? undefined : 'ltr', textAlign: 'start' }}>
            {label}{node.kind === 'domain' && new Set(node.flows.map((f) => f.addr)).size > 1 ? ` (${new Set(node.flows.map((f) => f.addr)).size})` : ''}
          </Text>
          <Text style={{ fontSize: 9, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
            {[upDownShort(node), node.kind !== 'country' && node.kind !== 'port' ? placeOf(tr, node.flows[0] || {}) : '',
              canSwitch ? verdictHint(tr, ctx, appOf, destName, destAddr, node.flows[0]?.port, node.flows[0]?.proto, node.listed)
                : node.listed ? tr('monitor.net.onList', { list: ctx.listName(node.listed) }) : ''].filter(Boolean).join(' · ')}
          </Text>
        </View>
        <Meter rx={node.rateRx} tx={node.rateTx} />
        {canSwitch ? <DestSwitch app={appOf} name={destName} addr={destAddr} port={node.flows[0]?.port} proto={node.flows[0]?.proto} listed={node.listed}
          mode={ctx.mode} restricted={ctx.restrictedOf(appOf)} /> : <View style={{ width: 22 }} />}
      </View>
      {open && list.map((c) => <Child key={c.key} node={c} depth={depth + 1} path={key} ctx={ctx} />)}
      {open && (more || ctx.all.has(key)) && kids.length > SHOWN ? (
        <View onClick={() => ctx.toggleAll(key)} style={{ height: 18, justifyContent: 'center', ...(direction() === 'rtl' ? { paddingRight: inset + 26 } : { paddingLeft: inset + 26 }) }}>
          <Text style={{ fontSize: 10, color: t.accent }}>{more ? tr('monitor.net.moreDomains', { count: more }) : tr('monitor.net.fewer')}</Text>
        </View>
      ) : null}
    </View>
  );
}

export function TreeChildren({ node, ctx }) {
  const t = useTheme();
  const key = node.key;
  const kids = node.children || [];
  const { list, more } = fold(kids, SHOWN, ctx.all.has(key));
  return (
    <View style={{ paddingBottom: 4, gap: 0 }}>
      {list.map((c) => <Child key={c.key} node={c} depth={1} path={key} ctx={ctx} />)}
      {kids.length > SHOWN ? (
        <View onClick={() => ctx.toggleAll(key)} style={{ height: 20, justifyContent: 'center', ...(direction() === 'rtl' ? { paddingRight: 40 } : { paddingLeft: 40 }) }}>
          <Text style={{ fontSize: 10, color: t.accent }}>{more ? ctx.tr('monitor.net.moreDomains', { count: more }) : ctx.tr('monitor.net.fewer')}</Text>
        </View>
      ) : null}
    </View>
  );
}
