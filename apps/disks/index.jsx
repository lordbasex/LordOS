// Disks — the LordOS disk utility, like GNOME Disks (see docs/SYSTEM.md,
// "Disks"). On the left, the disks and their partitions; on the right, usage, details, health
// (SMART) and the actions: mount, unmount, eject, format, rename, erase and create a bootable USB
// from an image.
//
// lord-server reads the list without root (lord.disks, refreshed by itself on mount and udev
// events). Anything that changes something goes through the helper with the closed disk-*
// actions, which validate again on the root side: never the system disk, and internal disks only
// for an administrator.
import {
  needsDiskFormats, openDiskFormats, View, Text, ScrollView, useTheme, useLocale, useSystemInfo, useAppWindow, admin, useDisks, disks, formatBytes,
  Toolbar as Bar, ToolbarButton, ToolbarSpacer, MenuButton, Hairline, GroupBox, FormRow, EmptyState, spacing, typeScale,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { DisksIcon, DriveGlyph, PartGlyph, fsColor } from './icons.jsx';
import { DiskDialog } from './dialogs.jsx';
import { HealthCard } from './health.jsx';

export function Icon({ size }) { return <DisksIcon size={size} />; }

const FS_NAMES = { hfsplus: 'HFS+', apfs: 'APFS', vfat: 'FAT32', exfat: 'exFAT', ntfs: 'NTFS', ext4: 'ext4', ext3: 'ext3', ext2: 'ext2', btrfs: 'Btrfs', xfs: 'XFS',
  swap: 'swap', iso9660: 'ISO 9660', udf: 'UDF', crypto_LUKS: 'LUKS', f2fs: 'F2FS' };
export const fsName = (fs) => FS_NAMES[fs] || fs;

export function diskTitle(d) { return d.model || d.name; }
export function partTitle(p, tr) { return p.label || p.partlabel || tr('disks.partNumber', { n: p.number }); }

export function useCanTouch() {
  const info = useSystemInfo(60000);
  const isAdmin = admin.available() && info.admin;
  return { isAdmin, can: (d) => !!d && !d.system && !d.ro && (d.removable || isAdmin) };
}

// Width of a box after layout (re-measured on every render, as the Player does).
function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => {
      if (!ref.current) return;
      const lw = Math.round(lord.getLayout(ref.current.id)[2]);
      if (lw > 0 && Math.abs(lw - w) > 2) setW(lw);
    }, 20);
    return () => clearTimeout(id);
  });
  return w;
}

// A key/value row of the information box: right-aligned label column, value cut at the edge.
function Row({ label, value }) {
  const t = useTheme();
  return (
    <FormRow label={label} labelWidth={150} minHeight={30}>
      <Text style={{ fontSize: typeScale.body, color: value ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{value || '—'}</Text>
    </FormRow>
  );
}

// Usage bar: a disk shows its partitions to scale; a partition, used and free space.
function UsageBar({ disk, part }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const track = { height: 16, borderRadius: 8, backgroundColor: t.control, overflow: 'hidden', flexDirection: 'row' };
  if (part) {
    const used = part.used, size = part.fssize || part.size;
    const frac = used != null && size ? Math.min(1, used / size) : null;
    return (
      <View style={{ gap: spacing.s }}>
        <View style={track}>
          {frac != null ? <View style={{ flexGrow: frac, backgroundColor: fsColor(part.fstype) }} /> : null}
          {frac != null ? <View style={{ flexGrow: 1 - frac }} /> : null}
        </View>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>
          {frac != null ? tr('disks.usedOf', { used: formatBytes(used), size: formatBytes(size), free: formatBytes(part.avail || 0) }) : tr('disks.notMountedUsage')}
        </Text>
      </View>
    );
  }
  const total = disk.size || 1;
  const parts = disk.partitions;
  const covered = parts.reduce((a, p) => a + p.size, 0);
  return (
    <View style={{ gap: spacing.s }}>
      <View style={[track, { gap: 1 }]}>
        {parts.length ? parts.map((p) => <View key={p.name} style={{ flexGrow: p.size / total, backgroundColor: fsColor(p.fstype) }} />)
          : disk.fstype ? <View style={{ flexGrow: 1, backgroundColor: fsColor(disk.fstype) }} /> : null}
        {parts.length && covered < total ? <View style={{ flexGrow: (total - covered) / total }} /> : null}
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.m }}>
        {parts.map((p) => (
          <View key={p.name} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <PartGlyph fstype={p.fstype} size={8} />
            <Text style={{ fontSize: typeScale.caption, color: t.text, whiteSpace: 'nowrap' }}>{partTitle(p, tr)}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap' }}>{formatBytes(p.size)}</Text>
          </View>
        ))}
        {parts.length && total - covered > 16 * 1048576 ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: t.control }} />
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('disks.unallocated', { size: formatBytes(total - covered) })}</Text>
          </View>
        ) : null}
      </View>
    </View>
  );
}

const transportName = (tr, d) => tr(`disks.bus.${['usb', 'sata', 'nvme', 'virtio', 'mmc', 'loop', 'scsi'].includes(d.transport) ? d.transport : 'scsi'}`);

// One row of the source list: a disk (two lines, bold) or one of its partitions (indented).
function SideRow({ selected, onClick, glyph, title, sub, indent }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 36, paddingVertical: spacing.xs, paddingHorizontal: 10,
        borderRadius: 6, backgroundColor: selected ? t.accent : hover ? t.hover : 'transparent' }}>
      {indent ? <View style={{ width: indent }} /> : null}
      <View style={{ width: 22, alignItems: 'center' }}>{glyph}</View>
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: indent ? 400 : 500, color: selected ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{title}</Text>
        <Text style={{ fontSize: typeScale.caption, color: selected ? 'rgba(255,255,255,0.8)' : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{sub}</Text>
      </View>
    </View>
  );
}

function Sidebar({ list, sel, onSel }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const groups = [
    [tr('disks.internal'), list.filter((d) => !d.removable && d.transport !== 'loop')],
    [tr('disks.external'), list.filter((d) => d.removable)],
    [tr('disks.images'), list.filter((d) => !d.removable && d.transport === 'loop')],
  ].filter(([, ds]) => ds.length);
  return (
    <View style={{ width: 240, backgroundColor: t.sidebarBg }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: 10, paddingTop: spacing.xs, paddingBottom: spacing.m }}>
          {groups.map(([title, ds]) => (
            <View key={title} style={{ gap: 2 }}>
              <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, paddingHorizontal: 10, marginTop: spacing.m, marginBottom: spacing.xs, textAlign: 'start' }}>{title}</Text>
              {ds.map((d) => (
                <View key={d.name} style={{ gap: 2 }}>
                  <SideRow selected={sel.disk === d.name && !sel.part} onClick={() => onSel({ disk: d.name, part: null })} glyph={<DriveGlyph disk={d} size={20} />}
                    title={diskTitle(d)} sub={`${formatBytes(d.size)} · ${transportName(tr, d)}${d.system ? ` · ${tr('disks.systemShort')}` : ''}`} />
                  {d.partitions.map((p) => (
                    <SideRow key={p.name} indent={14} selected={sel.part === p.name} onClick={() => onSel({ disk: d.name, part: p.name })} glyph={<PartGlyph fstype={p.fstype} size={12} />}
                      title={partTitle(p, tr)} sub={`${p.name} · ${formatBytes(p.size)}${p.mountpoint ? ` · ${p.mountpoint}` : ''}`} />
                  ))}
                </View>
              ))}
            </View>
          ))}
        </View>
      </ScrollView>
      <Hairline edge="end" />
    </View>
  );
}

// The toolbar over the details: the actions that apply to the selection as
// labelled toolbar items; the ones that don't fit go into the "⋯" menu, so the bar never wraps.
function DiskToolbar({ disk, part, onDialog, run, busy }) {
  const { t: tr } = useLocale();
  const t = useTheme();
  const { can } = useCanTouch();
  const box = useRef(null);
  const width = useWidth(box, 700);
  const ok = can(disk);
  const target = part || disk;
  const mountable = target.fstype && target.fstype !== 'swap' && target.fstype !== 'crypto_LUKS';
  const actions = [];
  if (ok && mountable && !target.mountpoint) actions.push(['mount', tr('disks.mount'), () => run('disk-mount', target.name, tr('disks.mounted'))]);
  if (ok && target.mountpoint) actions.push(['unmount', tr('disks.unmount'), () => run('disk-unmount', target.name, tr('disks.unmounted'))]);
  if (ok && !part && disk.removable) actions.push(['eject', tr('disks.eject'), () => run('disk-eject', disk.name, tr('disks.ejected'))]);
  if (ok) actions.push(['format', tr('disks.format'), () => onDialog({ kind: 'format' })]);
  if (ok && part && ['vfat', 'exfat', 'ext2', 'ext3', 'ext4', 'ntfs'].includes(part.fstype)) actions.push(['label', tr('disks.rename'), () => onDialog({ kind: 'label' })]);
  if (ok) actions.push(['erase', tr('disks.erase'), () => onDialog({ kind: 'erase' })]);
  if (ok && !part) actions.push(['image', tr('disks.makeBootable'), () => onDialog({ kind: 'image' })]);
  // what fits: label width (≈ 7 px per character at 13 px) + 16 px padding + 8 px gap
  const room = width - 2 * spacing.m - 36;
  const shown = [];
  let used = 0;
  for (const a of actions) {
    const need = a[1].length * 7 + 16 + spacing.s;
    if (used + need > room && shown.length < actions.length) break;
    used += need;
    shown.push(a);
  }
  const rest = actions.slice(shown.length);
  return (
    <View ref={box}>
      <Bar>
        {!ok ? (
          <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
            {disk.system ? tr('disks.systemNote') : disk.ro ? tr('disks.readOnlyNote') : tr('disks.adminNote')}</Text>
        ) : null}
        {shown.map(([k, label, fn]) => <ToolbarButton key={k} label={label} disabled={busy} onClick={fn} />)}
        {ok ? <ToolbarSpacer /> : null}
        {rest.length ? <MenuButton title={tr('disks.more')} items={rest.map(([, label, fn]) => ({ label, onClick: fn, disabled: busy }))} /> : null}
      </Bar>
    </View>
  );
}

function Details({ disk, part, onDialog, autoCheck }) {
  const t = useTheme();
  const { isAdmin } = useCanTouch();
  const { t: tr } = useLocale();
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  useEffect(() => setMsg(null), [disk.name, part && part.name]);
  const run = (action, arg, okText) => {
    setBusy(true);
    setMsg(null);
    return admin.run(action, arg, null, { quiet: true }).then(
      (r) => { setMsg({ ok: true, text: action === 'disk-mount' ? tr('disks.mountedAt', { path: r }) : okText }); },
      (e) => setMsg({ ok: false, text: e.message, formats: action === 'disk-mount' && needsDiskFormats((part || disk).fstype) })).finally(() => setBusy(false));
  };
  const target = part || disk;
  const title = part ? partTitle(part, tr) : diskTitle(disk);
  const subtitle = part
    ? [part.name, formatBytes(part.size), fsName(part.fstype) || tr('disks.noFs')].filter(Boolean).join(' · ')
    : [disk.name, formatBytes(disk.size), transportName(tr, disk), disk.table ? tr(`disks.table.${disk.table === 'gpt' ? 'gpt' : 'dos'}`) : ''].filter(Boolean).join(' · ');
  return (
    <View style={{ flex: 1, minWidth: 0 }}>
      <DiskToolbar disk={disk} part={part} onDialog={onDialog} run={run} busy={busy} />
      <ScrollView style={{ flex: 1 }}>
        <View style={{ padding: spacing.xl, gap: spacing.xl }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l }}>
            <View style={{ width: 52, alignItems: 'center' }}>{part ? <PartGlyph fstype={part.fstype} size={40} /> : <DriveGlyph disk={disk} size={52} />}</View>
            <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
              <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{title}</Text>
              <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{subtitle}</Text>
            </View>
            {disk.system ? <View style={{ paddingHorizontal: spacing.s, height: 22, justifyContent: 'center', borderRadius: 6, backgroundColor: t.control }}>
              <Text style={{ fontSize: typeScale.caption, fontWeight: 500, color: t.text }}>{tr('disks.systemBadge')}</Text></View> : null}
          </View>
          <UsageBar disk={disk} part={part} />
          {busy || msg ? (
            <Text style={{ fontSize: typeScale.callout, color: busy ? t.textDim : msg.ok ? '#30d158' : '#ff453a', textAlign: 'start' }}>
              {busy ? tr('disks.working') : msg.text}</Text>
          ) : null}
          {!busy && msg?.formats ? <View style={{ flexDirection: 'row' }}><Button small label={tr('disks.formatsGet')} onClick={openDiskFormats} /></View> : null}
          <GroupBox>
            {part ? [
              <Row key="dev" label={tr('disks.device')} value={`/dev/${part.name}`} />,
              <Row key="fs" label={tr('disks.fsType')} value={part.fstype ? `${fsName(part.fstype)}${part.fsversion ? ` (${part.fsversion})` : ''}` : tr('disks.noFs')} />,
              <Row key="label" label={tr('disks.label')} value={part.label} />,
              <Row key="uuid" label={tr('disks.uuid')} value={part.uuid} />,
              <Row key="pl" label={tr('disks.partName')} value={part.partlabel} />,
              <Row key="mp" label={tr('disks.mountPoint')} value={part.mountpoint || tr('disks.notMounted')} />,
              <Row key="off" label={tr('disks.offset')} value={formatBytes(part.start)} />,
            ] : [
              <Row key="dev" label={tr('disks.device')} value={`/dev/${disk.name}`} />,
              <Row key="model" label={tr('disks.model')} value={[disk.vendor, disk.model].filter(Boolean).join(' ')} />,
              <Row key="serial" label={tr('disks.serial')} value={disk.serial} />,
              <Row key="cap" label={tr('disks.capacity')} value={tr('disks.bytes', { size: formatBytes(disk.size), n: disk.size })} />,
              <Row key="bus" label={tr('disks.connection')} value={transportName(tr, disk)} />,
              <Row key="kind" label={tr('disks.kind')} value={disk.transport === 'loop' ? tr('disks.kindImage') : disk.removable ? tr('disks.kindRemovable') : disk.rotational ? tr('disks.kindHdd') : tr('disks.kindSsd')} />,
              <Row key="table" label={tr('disks.partTable')} value={disk.table ? tr(`disks.table.${disk.table === 'gpt' ? 'gpt' : 'dos'}`) : tr('disks.noTable')} />,
              <Row key="parts" label={tr('disks.partitions')} value={String(disk.partitions.length)} />,
            ]}
          </GroupBox>
          {!part ? <HealthCard disk={disk} isAdmin={isAdmin} autoCheck={autoCheck} /> : null}
          {target.ro ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('disks.readOnlyNote')}</Text> : null}
        </View>
      </ScrollView>
    </View>
  );
}

export default function Disks() {
  const { t: tr } = useLocale();
  const [raw, reload] = useDisks();
  const list = useMemo(() => [...raw].sort((a, b) => a.name.localeCompare(b.name)), [raw]);
  const [sel, setSel] = useState({ disk: null, part: null });
  // "Open in Disks" from the Monitor or a notice: that disk selected
  const { args } = useAppWindow() || {};
  useEffect(() => { if (args?.disk) setSel({ disk: args.disk, part: null }); }, [args?.disk, args?.nonce]);
  const [dialog, setDialog] = useState(null);
  const tools = useMemo(() => disks.tools(), []);
  // pick something on open (the first removable, if any) and keep a valid selection if it disappears
  const disk = list.find((d) => d.name === sel.disk) || list.find((d) => d.removable) || list[0];
  const part = disk && sel.part ? disk.partitions.find((p) => p.name === sel.part) || null : null;
  if (!disks.available()) return <EmptyState mascot="confused" title={tr('disks.unavailable')} />;
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <Sidebar list={list} sel={{ disk: disk && disk.name, part: part && part.name }} onSel={setSel} />
      {disk ? <Details key={disk.name} disk={disk} part={part} onDialog={(d) => setDialog({ ...d, disk, part })} autoCheck={args?.disk === disk.name ? args.nonce || 1 : null} />
        : <EmptyState icon={<DisksIcon size={56} />} title={tr('disks.none')} />}
      {dialog ? <DiskDialog {...dialog} tools={tools} onClose={() => { setDialog(null); reload(); }} /> : null}
    </View>
  );
}
