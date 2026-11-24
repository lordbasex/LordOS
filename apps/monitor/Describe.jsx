// Describe.jsx — Get Info's "Description" (docs/MONITOR.md, "Descriptions") and "Investigate with Lumo"
// (docs/SENTINELA.md, "Investigate"), between the command line and the tabs.
//
// The description comes from sdk/describe.js (describeProgram): what LordOS knows of well-known programs and its own,
// a LordOS app's manifest, a .desktop or Flatpak app's Comment, the program's manual page, a service's Description=, or
// its package's description (in the person's language when Debian's translation is installed). English when nothing
// else exists, with "Translate with Lumo" while Sentinela is on (kept per language). Nothing known: "Unknown" and, with
// Sentinela on and a local model installed, "Investigate with Lumo": on that click only, the local model reads the facts
// and the program's script text or a binary's metadata (read by lord-server, fenced as data) and answers what it does,
// why it probably runs, a risk and a suggested action; the actions (End, Pause, Block its network, Blocklist) run only
// on the person's click, only on their own processes.
import {
  View, Text, Button, typeScale, spacing, useTheme, useLocale, useStore, useLauncherApps, monitor, describeProgram, saveLumoTranslation,
  translationKey, lumoReady, investigateProcess, translateWithLumo, runInvestigateAction, sentinelaPrefs, settings, wm, errorText,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { exePath } from './procgroupmodel.js';

const RISK_COLOR = { low: '#30d158', medium: '#ff9f0a', high: '#ff453a' };

// how it started, from Get Info's cgroup and the chain: a system or user service, the Terminal, an app or the session
export function startedOf(d, p) {
  const cg = String(d?.cgroup || '');
  const unit = cg.split('/').filter(Boolean).pop() || '';
  if (/^cron\.service$/.test(unit) || /\/cron\.service\//.test(cg)) return 'cron';
  if (/autostart/.test(unit)) return 'autostart';
  if (/\.service$/.test(unit) && !/^(user@|session-)/.test(unit)) return `unit:${unit}`;
  if (p?.app === 'lordos:terminal') return 'terminal';
  if (p?.app && p.app !== 'lordos') return 'app';
  return 'session';
}
// the unit whose Description= may say what it is: a service of the system's or the user's
export const unitOf = (d) => { const u = String(d?.cgroup || '').split('/').filter(Boolean).pop() || ''; return /^[\w:.@\\-]+\.service$/.test(u) && !/^user@/.test(u) ? u : null; };

function Line({ label, children }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', gap: spacing.s }}>
      <Text style={{ width: 110, fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{label}</Text>
      <View style={{ flex: 1, minWidth: 0, gap: 3 }}>{children}</View>
    </View>
  );
}

export function DescriptionPanel({ p, d, own }) {
  const t = useTheme();
  const { t: tr, lang } = useLocale();
  const apps = useLauncherApps({ all: true });
  useStore(settings, (s) => s.sentinela?.on);
  const [tick, setTick] = useState(0);
  const [trans, setTrans] = useState(null);           // null | 'running' | { error }
  const [inv, setInv] = useState(null);               // null | 'running' | { ok, result, evidence } | { error }
  const [done, setDone] = useState(null);             // { action, ok, error }
  const [confirm, setConfirm] = useState(false);
  const exe = exePath(p?.exe);
  const owner = exe ? monitor.ownerOf(exe) : null;           // undefined while lord-server looks it up
  const pkg = owner || null;
  useEffect(() => { if (exe && owner === undefined) monitor.askOwners([exe]); }, [exe, tick]);
  const desc = p ? describeProgram({ ...p, pkg, unit: unitOf(d) }, { lang, apps }) : { unknown: true };
  // lord-server reads in its worker thread: asked again a moment later, a few times
  useEffect(() => {
    if (!desc.pending && !desc.more && owner !== undefined) return undefined;
    if (tick > 40) return undefined;
    const id = setTimeout(() => setTick((n) => n + 1), 250);
    return () => clearTimeout(id);
  });
  if (!p) return null;
  const ready = lumoReady();
  const enableNote = (
    <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>
      {tr(ready.why === 'off' ? 'monitor.desc.lumoOff' : 'monitor.desc.lumoNoModel')}
      {' '}
      <Text onClick={() => wm.open('settings', { args: { section: 'ai', page: 'sentinela', nonce: Date.now() } })} style={{ fontSize: typeScale.caption, color: t.accent }}>{tr('monitor.desc.openSettings')}</Text>
    </Text>
  );
  const translate = async () => {
    setTrans('running');
    const r = await translateWithLumo(desc.text);
    if (r.ok) { saveLumoTranslation(translationKey(desc), lang, r.text); setTrans(null); setTick((n) => n + 1); } else setTrans({ error: r.error });
  };
  const investigate = async () => {
    setInv('running');
    setDone(null);
    setConfirm(false);
    // its network sockets, from Get Info's open files (counts only: no address goes to the model)
    const socks = Array.isArray(d?.fds) ? d.fds.filter((f) => f.k === 'socket' && f.proto && f.proto !== 'UNIX') : null;
    const conns = socks ? { total: socks.length, remote: socks.filter((f) => f.remote).length } : null;
    setInv(await investigateProcess(p, { started: startedOf(d, p), conns }));
  };
  const act = async (action) => {
    if (action === 'blocklist' && !confirm) { setConfirm(true); return; }
    setConfirm(false);
    const r = await runInvestigateAction(p, action);
    setDone({ action, ...r });
  };
  const srcText = desc.src === 'lumo' ? tr('monitor.desc.src.lumo') : desc.english ? tr('monitor.desc.english') : null;
  return (
    <View style={{ gap: 6 }}>
      <Line label={tr('monitor.desc.label')}>
        {desc.pending ? <Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'start' }}>{tr('monitor.loading')}</Text>
          : desc.unknown ? <Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'start' }}>{tr('monitor.desc.unknown')}</Text>
            : <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start', lineHeight: 1.35 }} numberOfLines={3}>{desc.text}</Text>}
        {!desc.pending && !desc.unknown && desc.pkgText ? (
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }} numberOfLines={2}>{tr('monitor.desc.fromPkg', { pkg: desc.pkg, text: desc.pkgText })}</Text>
        ) : null}
        {srcText ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, flexWrap: 'wrap' }}>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{srcText}</Text>
            {desc.english && ready.ok && trans !== 'running' ? <Button small label={tr('monitor.desc.translate')} onClick={translate} /> : null}
            {trans === 'running' ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('monitor.desc.translating')}</Text> : null}
            {trans && trans.error ? <Text style={{ fontSize: typeScale.caption, color: RISK_COLOR.high }}>{errorText(trans.error)}</Text> : null}
          </View>
        ) : null}
        {desc.unknown && own && inv == null ? (ready.ok ? <View style={{ flexDirection: 'row' }}><Button small label={tr('monitor.desc.investigate')} onClick={investigate} /></View> : enableNote) : null}
        {desc.unknown && !own && inv == null ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('monitor.desc.notOwn')}</Text> : null}
      </Line>
      {inv === 'running' ? (
        <Line label={tr('monitor.desc.lumo')}><Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'start' }}>{tr('monitor.desc.investigating')}</Text></Line>
      ) : inv && !inv.ok ? (
        <Line label={tr('monitor.desc.lumo')}>
          <Text style={{ fontSize: typeScale.body, color: RISK_COLOR.high, textAlign: 'start' }}>{errorText(inv.error)}</Text>
          <View style={{ flexDirection: 'row' }}><Button small label={tr('monitor.desc.again')} onClick={investigate} /></View>
        </Line>
      ) : inv && inv.ok ? (
        <Line label={tr('monitor.desc.lumo')}>
          <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start', lineHeight: 1.35 }} numberOfLines={4}>{inv.result.what}</Text>
          {inv.result.why ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }} numberOfLines={3}>{inv.result.why}</Text> : null}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, flexWrap: 'wrap' }}>
            <View style={{ paddingHorizontal: 6, height: 17, borderRadius: 4, justifyContent: 'center', backgroundColor: `${RISK_COLOR[inv.result.risk]}26`, border: `1px solid ${RISK_COLOR[inv.result.risk]}66` }}>
              <Text style={{ fontSize: 10.5, fontWeight: 600, color: RISK_COLOR[inv.result.risk] }}>{tr(`monitor.desc.risk.${inv.result.risk}`)}</Text>
            </View>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(`monitor.desc.read.${inv.evidence.kind}`, { interp: inv.evidence.interp || '', kb: Math.max(1, Math.round(inv.evidence.bytes / 1024)), libs: inv.evidence.libs })}</Text>
          </View>
          {done ? (
            <Text style={{ fontSize: typeScale.caption, color: done.ok ? t.textDim : RISK_COLOR.high, textAlign: 'start' }}>
              {done.ok ? tr(`monitor.desc.done.${done.action}`) : errorText(done.error)}</Text>
          ) : confirm ? (
            <View style={{ gap: 4 }}>
              <Text style={{ fontSize: typeScale.caption, color: t.text, textAlign: 'start', lineHeight: 1.35 }}>{tr('monitor.desc.blockConfirm', { name: p.name })}</Text>
              <View style={{ flexDirection: 'row', gap: spacing.s }}>
                <Button small label={tr('monitor.proc.cancel')} onClick={() => setConfirm(false)} />
                <Button small destructive label={tr('monitor.desc.action.blocklist')} onClick={() => act('blocklist')} />
              </View>
            </View>
          ) : (
            <View style={{ flexDirection: 'row', gap: spacing.s, flexWrap: 'wrap' }}>
              {['end', 'pause', 'block_network', 'blocklist'].map((a) => (
                <Button key={a} small primary={inv.result.action === a} destructive={a === 'end' || a === 'blocklist'} label={tr(`monitor.desc.action.${a}`)}
                  disabled={a === 'block_network' && !p.app} onClick={() => act(a)} />
              ))}
            </View>
          )}
        </Line>
      ) : null}
    </View>
  );
}

// for the tests: Sentinela's switch (Settings → AI → Sentinela)
export const lumoState = () => ({ on: !!sentinelaPrefs().on, ready: lumoReady() });
