// Settings → Graphics cards (docs/MACHINES-GPU.md, "Settings → Graphics cards"; AGENTS.md "Machines' graphics cards"):
// shown only on a computer with more than one GPU. Every GPU is listed with what uses it; the one drawing this desktop is
// fixed. A spare Intel iGPU that GVT-g supports can be Unused or shared with Machines (Mode A): the closed admin action
// gpu-role writes LordOS's fixed module files and the initramfs, and the change takes effect after a restart. Other
// spare cards get no option yet (Machines: dedicated comes later). lord.vm.gpus() is read here; root checks everything
// again (vmgpu_admin.c).
import { View, Text, useTheme, useLocale, Segmented, Button, Stepper, admin, session, useSystemInfo, spacing, typeScale } from '@lordos/sdk';
import { useState } from 'react';
import { GroupBox, SettingRow, ValueRow, Note, Stack, StatusDot, tint } from './ui.jsx';

const INTEL = 0x8086;
const ROLES = ['unused', 'machines-shared'];

// lord.vm.gpus(), or null where this lord-server doesn't have it
export function readGpus() {
  try { return typeof lord.vm?.gpus === 'function' ? lord.vm.gpus() : null; } catch (e) { return null; }
}
// the section shows only with more than one GPU: a single one is always the desktop's, with nothing to choose
export const manyGpus = () => (readGpus()?.gpus?.length || 0) > 1;

const hex4 = (v) => ((v >>> 0) & 0xffff).toString(16).padStart(4, '0');
// what a GPU can be given: 'desktop' (fixed), 'shared' (Unused / Machines: shared), 'noGvt' (an Intel iGPU GVT-g doesn't
// support) or 'later' (another maker's card: dedicated machines come in a later version)
export function gpuKind(g) {
  if (g.desktop) return 'desktop';
  if (g.vendor === INTEL) return g.gvtCapable ? 'shared' : 'noGvt';
  return 'later';
}

// "Memory machines may lock" (maintainer, 2026-11-21): a machine with a graphics card has its memory locked in RAM while
// it runs (VFIO), and all such machines together stay within this cap. gpu-memlock-cap writes it (or "default" removes
// it); it applies at once. memlock: lord.vm.gpus()'s { capMb, defaultMb, totalMb, minMb, maxMb, custom }
function MemlockBox({ memlock, isAdmin, onDone }) {
  const { t: tr } = useLocale();
  const minGb = Math.ceil((memlock.minMb | 0) / 1024), maxGb = Math.max(minGb, Math.floor((memlock.maxMb | 0) / 1024));
  const capGb = Math.floor((memlock.capMb | 0) / 1024), defGb = Math.floor((memlock.defaultMb | 0) / 1024);
  const [want, setWant] = useState(null);       // the value being chosen, in GB; null: the current cap
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  const shown = want ?? capGb;
  const run = (arg) => {
    setBusy(true);
    setErr('');
    admin.run('gpu-memlock-cap', arg, null, { quiet: true }).then(() => setWant(null), (e) => setErr(e.message))
      .finally(() => { setBusy(false); onDone(); });
  };
  const gb = (n) => tr('settings.gpus.gb', { n });
  return (
    <GroupBox title={tr('settings.gpus.memlock')} footer={err || tr('settings.gpus.memlockHint', { total: gb(Math.round((memlock.totalMb | 0) / 1024)) })}>
      <SettingRow label={tr('settings.gpus.memlockCap')} hint={shown === defGb ? tr('settings.gpus.memlockRecommended') : tr('settings.gpus.memlockDefault', { n: gb(defGb) })}
        disabled={!isAdmin || busy}>
        <Stepper value={shown} min={minGb} max={maxGb} step={1} width={80} format={gb} onChange={(v) => isAdmin && setWant(v)} />
        {want != null && want !== capGb ? <Button small primary label={tr('settings.gpus.memlockApply')} disabled={!isAdmin || busy} onClick={() => run(String(want * 1024))} /> : null}
        {memlock.custom ? <Button small label={tr('settings.gpus.memlockUseDefault')} disabled={!isAdmin || busy} onClick={() => run('default')} /> : null}
      </SettingRow>
    </GroupBox>
  );
}

export function GpusSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const info = useSystemInfo(60000);
  const [r, setR] = useState(readGpus);
  const [busy, setBusy] = useState('');          // the address whose role is being written
  const [msg, setMsg] = useState(null);          // { ok, text }
  const [changed, setChanged] = useState(false); // a role written in this visit: restart to apply
  const isAdmin = !!info.admin && admin.available();
  const canRestart = typeof session.canPower === 'function' && session.canPower();

  if (!r) return <Stack><Note>{tr('settings.gpus.unavailable')}</Note></Stack>;
  const gpus = r.gpus || [];
  const configured = ROLES.includes(r.configured) ? r.configured : 'unused';

  const setRole = (g, role) => {
    if (busy || role === configured) return;
    setBusy(g.address);
    setMsg(null);
    admin.run('gpu-role', `${g.address} ${role}`, null, { quiet: true }).then(() => {
      setChanged(true);
      setMsg(null);
    }, (e) => setMsg({ ok: false, text: e.message })).finally(() => { setBusy(''); setR(readGpus()); });
  };

  const name = (g) => (g.vendorName ? tr('settings.gpus.name', { vendor: g.vendorName, id: `${hex4(g.vendor)}:${hex4(g.device)}` })
    : tr('settings.gpus.nameUnknown', { id: `${hex4(g.vendor)}:${hex4(g.device)}` }));
  // a spare iGPU nobody uses: suggested at the top
  const spare = gpus.find((g) => gpuKind(g) === 'shared' && configured === 'unused');
  const restart = changed || !!r.restart;

  return (
    <Stack>
      {spare && !restart ? (
        <GroupBox>
          <SettingRow label={tr('settings.gpus.suggest')} hint={tr('settings.gpus.suggestHint', { name: name(spare) })}>
            <Button small primary label={tr('settings.gpus.suggestUse')} disabled={!isAdmin || !!busy} onClick={() => setRole(spare, 'machines-shared')} />
          </SettingRow>
        </GroupBox>
      ) : null}
      {restart ? (
        <GroupBox>
          <SettingRow label={tr('settings.gpus.restart')} hint={tr('settings.gpus.restartHint')}>
            {canRestart ? <Button small primary label={tr('settings.gpus.restartNow')} onClick={() => session.restart()} /> : null}
          </SettingRow>
        </GroupBox>
      ) : null}
      {gpus.map((g) => {
        const kind = gpuKind(g);
        const role = configured;
        return (
          <GroupBox key={g.address} title={name(g)}
            footer={kind === 'shared' ? `${tr('settings.gpus.unusedHint')} ${tr('settings.gpus.sharedHint')}`
              : kind === 'desktop' ? tr('settings.gpus.desktopHint') : kind === 'noGvt' ? tr('settings.gpus.noGvt') : tr('settings.gpus.later')}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: spacing.m }}>
              <StatusDot color={kind === 'desktop' ? tint.ok : kind === 'shared' && (g.gvtOn || role === 'machines-shared') ? tint.ok : tint.off} size={10} />
              <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>
                {kind === 'desktop' ? tr('settings.gpus.desktop')
                  : kind === 'shared' ? tr(g.gvtOn ? 'settings.gpus.nowShared' : 'settings.gpus.nowUnused') : tr('settings.gpus.nowUnused')}</Text>
            </View>
            <ValueRow label={tr('settings.gpus.address')} value={g.address} mono />
            <ValueRow label={tr('settings.gpus.driver')} value={g.driver || tr('settings.gpus.noDriver')} />
            {kind === 'shared' ? (
              <SettingRow label={tr('settings.gpus.role')} stack disabled={!isAdmin || !!busy}>
                <Segmented small value={role} onChange={(v) => isAdmin && setRole(g, v)}
                  options={[['unused', tr('settings.gpus.roleUnused')], ['machines-shared', tr('settings.gpus.roleShared')]]} />
              </SettingRow>
            ) : null}
            {busy === g.address ? <Note>{tr('settings.gpus.applying')}</Note> : null}
          </GroupBox>
        );
      })}
      {r.memlock && gpus.some((g) => gpuKind(g) === 'shared') ? <MemlockBox memlock={r.memlock} isAdmin={isAdmin} onDone={() => setR(readGpus())} /> : null}
      {!isAdmin && gpus.some((g) => gpuKind(g) === 'shared') ? <Note>{tr('settings.gpus.needAdmin')}</Note> : null}
      {msg ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null}
    </Stack>
  );
}
