// Machines settings (the approved mocks "Settings", "Systems", "ProfileEdit" and "ProfileImport", 2026-11-22): its own
// window, opened by the library's gear and the app menu. Sections: General (the library, closing, tools, the official
// logos switch), Profiles (LordOS's five and the person's own: new, duplicate, edit, delete, default, import and export
// as .lordprofile), Systems (the catalog with its state, and the systems the person adds: an ISO on this computer or an
// https address with its SHA-256), Network (the card bridged machines join), Graphics cards (what Settings → Graphics
// cards set up) and Keyboard and mouse. docs/VIRTUALIZATION.md "Profiles and your own systems".
import { View, Text, TextInput, TextArea, Toggle, Segmented, Slider, ScrollView, FilePicker, ConfirmDialog, useAppWindow, useSettings, useStore, settings, fs, wm,
  perms, useNarrow, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import CATALOG from '../../assets/machines/systems.json';
import { systemsFor, unattendedOf } from './catalog.js';
import { LIB_VIEWS, LIB_SORTS, libView, NET_MODES, GPU_CHOICES, PORT_HOST_MIN, newPortRule, portProblems, portInput, usesPorts, CLOSE_CHOICES, gvtGpu, no3dReason, missingFor, vcpuDefault } from './model.js';
import { BUILTIN, DEFAULT_PROFILE, CLIPBOARDS, MOUSE, SHORTCUTS, allProfiles, builtinProfile, blankProfile, readProfile, writeProfile, onHost, cpuCap, memCap,
  defaultProfileId, exportName, shareRel, exposureOf, PROFILE_FILE_MAX } from './profiles.js';
import { CARD_COLORS, COLOR_HEX, SYSTEM_KINDS, INSTALLS, validateSystem, newId, systemState, signatureState, fileOfUrl, hostOf, keyOf, KEY_MAX } from './systems.js';
import { loadProfiles, saveProfile, deleteProfile, loadSystems, saveSystems, defaultsOf, setDefaults, removeLogos, storeRev, profileTitle } from './store.js';
import { fetchAllLogos, logoSystems } from './logos.js';
import { downloadSystem, downloadedIds, stopDownload, downloadUserSystem, checkSignature } from './download.js';
import { usePal, has, gb, bytes, Btn, Bar, MenuBtn, OsMark } from './ui.jsx';
import { Sheet, SheetHeader, Pill, Section, Round } from './kit.jsx';
import { Requirements, openStorage } from './library.jsx';

const SECTIONS = ['general', 'profiles', 'systems', 'network', 'gpus', 'input'];
const readGpus = () => { try { return typeof lord.vm?.gpus === 'function' ? lord.vm.gpus() : null; } catch (e) { return null; } };
const hostOf0 = () => (has() ? lord.vm.host() : null);
const nowIso = () => new Date().toISOString();
// a fingerprint as projects print it: groups of four
const fprText = (f) => String(f || '').replace(/(.{4})(?=.)/g, '$1 ');

// a row of a card: label at the start, value at the end, an optional note and control under them
function Row({ label, value, children, note, last }) {
  const p = usePal();
  return (
    <View style={{ paddingHorizontal: 16, paddingVertical: 12, gap: 8, borderBottomWidth: last ? 0 : 1, borderColor: p.line }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ flex: 1, fontSize: 14, color: p.text, textAlign: 'start' }}>{label}</Text>
        {value != null ? <Text style={{ fontSize: 14, fontWeight: 600, color: p.text, textAlign: 'end' }}>{value}</Text> : null}
      </View>
      {children}
      {note ? <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{note}</Text> : null}
    </View>
  );
}
function Head({ title, text, children }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 12 }}>
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {text ? <Text style={{ marginTop: 6, fontSize: 13, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{text}</Text> : null}
      </View>
      {children}
    </View>
  );
}
const yesNo = (v) => tr(v ? 'machines.set.yes' : 'machines.set.no');
// a profile's name and its tile's second line
const profileHint = (pr, def) => (pr.id === def ? tr('machines.set.prof.default') : pr.builtin ? tr(`machines.set.prof.hint.${pr.id}`) : pr.description || pr.author || tr('machines.set.prof.yours'));
const gpuText = (g) => (!g ? tr('machines.cfg.gpuNone') : g === 'intel-whole' ? tr('machines.set.gpu.whole') : tr('machines.set.gpu.shared'));
const kindText = (k) => (k === 'windows' ? tr('machines.win.sys.kind') : tr(`machines.set.sys.kind.${k}`));

// ---- the window ----

export function MachinesSettings({ section }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [sec, setSec] = useState(SECTIONS.includes(section) ? section : 'profiles');
  useEffect(() => { if (SECTIONS.includes(section)) setSec(section); }, [section]);
  useEffect(() => { wm.setTitle(winId, tr('machines.set.title')); }, []);
  const [host] = useState(hostOf0);
  const [gpus] = useState(readGpus);
  const narrow = useNarrow(760);
  const g = gvtGpu(gpus);
  const hint = (k) => (k === 'gpus' ? (g ? tr('machines.set.nav.gpusFor', { name: g.name || tr('machines.set.gpus.intel') }) : tr('machines.set.nav.gpusNone')) : tr(`machines.set.nav.${k}Hint`));
  if (!host) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.noBackend')}</Text></View>;
  const pane = sec === 'general' ? <General host={host} /> : sec === 'profiles' ? <Profiles host={host} gpus={gpus} /> : sec === 'systems' ? <Systems host={host} />
    : sec === 'network' ? <Network host={host} /> : sec === 'gpus' ? <Gpus gpus={gpus} /> : <Input />;
  return (
    <View style={{ flex: 1, flexDirection: narrow ? 'column' : 'row', backgroundColor: p.bg }}>
      <View style={{ width: narrow ? undefined : 250, flexDirection: narrow ? 'row' : 'column', flexWrap: narrow ? 'wrap' : 'nowrap', paddingHorizontal: 10, paddingVertical: narrow ? 8 : 16, gap: 2,
        backgroundColor: p.dark ? 'rgba(0,0,0,0.12)' : 'rgba(0,0,0,0.025)', borderEndWidth: narrow ? 0 : 1, borderBottomWidth: narrow ? 1 : 0, borderColor: p.line }}>
        {SECTIONS.map((k) => (
          <View key={k} onClick={() => setSec(k)} style={{ paddingHorizontal: 12, paddingVertical: narrow ? 6 : 10, borderRadius: 10, backgroundColor: sec === k ? p.accent : 'transparent' }}>
            <Text style={{ fontSize: 14, fontWeight: 600, color: sec === k ? '#fff' : p.text, textAlign: 'start' }}>{tr(`machines.set.nav.${k}`)}</Text>
            {narrow ? null : <Text style={{ fontSize: 12, color: sec === k ? 'rgba(255,255,255,0.85)' : p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{hint(k)}</Text>}
          </View>
        ))}
      </View>
      <ScrollView style={{ flex: 1, minWidth: 0 }} contentStyle={{ paddingHorizontal: narrow ? 16 : 32, paddingVertical: 24, gap: 18 }} resetKey={sec}>{pane}</ScrollView>
    </View>
  );
}

// ---- General ----

function General({ host }) {
  const p = usePal();
  const [s0] = useSettings();
  const lv = libView(s0.machinesView);
  const d = defaultsOf(s0.machinesDefaults);
  const [ask, setAsk] = useState(false);
  const [req, setReq] = useState(false);
  const [msg, setMsg] = useState('');
  const putView = (patch) => settings.set({ machinesView: { ...lv, ...patch } });
  const own = loadProfiles();
  const def = allProfiles(own, host).find((x) => x.id === defaultProfileId(d.profile, own)) || builtinProfile(DEFAULT_PROFILE, host);
  const missing = missingFor(host, [host.arch]);
  const avail = logoSystems();
  return <>
    <Head title={tr('machines.set.nav.general')} />
    <Section title={tr('machines.set.gen.library')}>
      <Row label={tr('machines.set.gen.view')}>
        <View style={{ alignSelf: 'flex-start' }}><Segmented small value={lv.view} onChange={(v) => putView({ view: v })}
          options={LIB_VIEWS.map((k) => [k, tr(k === 'grid' ? 'machines.lib.viewGrid' : 'machines.lib.viewList')])} /></View>
      </Row>
      <Row label={tr('machines.lib.sort')} last>
        <View style={{ alignSelf: 'flex-start' }}><Segmented small value={lv.sort} onChange={(v) => putView({ sort: v })} options={LIB_SORTS.map((k) => [k, tr(`machines.lib.sort.${k}`)])} /></View>
      </Row>
    </Section>
    <Section title={tr('machines.set.gen.closing')}>
      <Row label={tr('machines.set.gen.onClose')} value={tr(`machines.cfg.close.${def.integration.onClose}`)} last note={tr('machines.set.gen.onCloseNote', { profile: profileTitle(def) })} />
    </Section>
    <Section title={tr('machines.set.gen.tools')}>
      <Row label={tr('machines.storage.menu')}><View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.set.open')} onClick={openStorage} /></View></Row>
      <Row label={tr('machines.req.menu')} value={missing.length ? tr('machines.set.gen.reqMissing', { n: missing.length }) : tr('machines.set.gen.reqReady')}>
        <View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.set.open')} onClick={() => setReq(true)} /></View>
      </Row>
      {/* the default for every machine whose Configure → Agent doesn't say (model.js toolsAutoOn) */}
      <Row label={tr('machines.set.gen.toolsAuto')} last note={tr('machines.set.gen.toolsAutoNote')}>
        <View style={{ alignSelf: 'flex-start' }}><Toggle on={d.toolsAuto} onChange={(on) => setDefaults({ toolsAuto: on })} /></View>
      </Row>
    </Section>
    <Section title={tr('machines.set.logos.title')}>
      <Row label={tr('machines.set.logos.switch')} note={avail.length ? tr('machines.set.logos.which', { list: avail.map((x) => x.name).join(', ') }) : tr('machines.set.logos.none')}>
        <View style={{ alignSelf: 'flex-start' }}><Toggle on={d.logos} onChange={(on) => { if (on) setAsk(true); else { setDefaults({ logos: false }); setMsg(''); } }} /></View>
      </Row>
      <Row label={tr('machines.set.logos.remove')} last note={msg || null}>
        <View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.set.logos.removeBtn')} onClick={() => { const n = removeLogos(); setMsg(tr('machines.set.logos.removed', { n })); }} /></View>
      </Row>
    </Section>
    <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.set.logos.trademarks')}</Text>
    {ask ? <ConfirmDialog title={tr('machines.set.logos.askTitle')} detail={tr('machines.set.logos.notice')} okLabel={tr('machines.set.logos.turnOn')} cancelLabel={tr('machines.cancel')}
      onOk={() => { setAsk(false); setDefaults({ logos: true }); fetchAllLogos(host.arch); }} onCancel={() => setAsk(false)} /> : null}
    {req ? <Sheet width={760} height={600} onClose={() => setReq(false)}><View style={{ flex: 1, padding: 18 }}><Requirements host={host} onBack={() => setReq(false)} onChanged={() => {}} /></View></Sheet> : null}
  </>;
}

// ---- Profiles ----

function ProfileValues({ pr, host, gpus }) {
  const g = !!gvtGpu(gpus);
  const v = onHost(pr, host, { gpuReady: g, no3d: no3dReason(host, false, host.arch) });
  const half = pr.resources.cpus === 'half';
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 16 }}>
      <View style={{ flex: 1, minWidth: 280 }}>
        <Section title={tr('machines.set.prof.resources')}>
          <Row label={tr('machines.set.prof.cpus')} value={tr('machines.set.prof.cpusOf', { n: v.vcpus, total: host.cpus || '?' })} note={half ? tr('machines.set.prof.half') : null} />
          <Row label={tr('machines.as.r.memory')} value={gb(v.memoryMb)} />
          <Row label={tr('machines.as.r.disk')} value={tr('machines.set.prof.diskGrows', { n: v.diskGb })} />
          <Row label={tr('machines.cfg.graphics')} value={pr.resources.graphics === '3d' ? tr('machines.set.prof.gfx3d') : tr('machines.cfg.graphics2d')} />
          <Row label={tr('machines.set.prof.gpu')} value={gpuText(v.gpu)} note={pr.resources.gpu && !g ? tr('machines.set.prof.gpuNotHere') : null} last />
        </Section>
      </View>
      <View style={{ flex: 1, minWidth: 280 }}>
        <Section title={tr('machines.set.prof.integration')}>
          <Row label={tr('machines.as.r.network')} value={tr(`machines.cfg.net.${v.network}`)} />
          <Row label={tr('machines.set.prof.clipboard')} value={tr(`machines.cfg.clip.${v.clipboard}`)} />
          <Row label={tr('machines.cfg.transfer')} value={v.transfer ? tr('machines.set.on') : tr('machines.set.off')} />
          <Row label={tr('machines.set.prof.appsMode')} value={yesNo(v.appsMode)} />
          <Row label={tr('machines.set.gen.onClose')} value={tr(`machines.cfg.close.${v.onClose}`)} last />
        </Section>
      </View>
    </View>
  );
}

function Profiles({ host, gpus }) {
  const p = usePal();
  const [s0] = useSettings();
  const rev = useStore(storeRev);
  const d = defaultsOf(s0.machinesDefaults);
  const [own, setOwn] = useState(loadProfiles);
  useEffect(() => { setOwn(loadProfiles()); }, [rev]);
  const def = defaultProfileId(d.profile, own);
  const list = allProfiles(own, host);
  const [selId, setSelId] = useState(def);
  const sel = list.find((x) => x.id === selId) || list[0];
  const [edit, setEdit] = useState(null);           // { profile, fresh }
  const [del, setDel] = useState(null);
  const [picking, setPicking] = useState(false);
  const [imp, setImp] = useState(null);             // { file, r }
  const [err, setErr] = useState('');
  const ids = list.map((x) => x.id);
  const fresh = (from) => setEdit({ fresh: true, profile: { ...blankProfile(from, newId('u', ids)), name: from ? tr('machines.set.prof.copyOf', { name: profileTitle(from) }) : '' } });
  const doImport = (path) => {
    setErr('');
    try {
      const st = fs.stat(path);
      if (!st || st.size > PROFILE_FILE_MAX) { setErr(tr('machines.set.imp.err.tooBig')); return; }
      const systems = loadSystems();
      const known = [...systemsFor(CATALOG, host.arch).map((x) => x.id), ...systems.map((x) => x.id)];
      const r = readProfile(fs.read(path), { imported: true, host, gpuReady: !!gvtGpu(gpus), systems: known, home: fs.home(), id: newId('u', ids), imageId: newId('s', systems.map((x) => x.id)) });
      if (!r.ok) { setErr(tr(`machines.set.imp.err.${r.error}`)); return; }
      setImp({ file: path.split('/').pop(), r });
    } catch (e) { setErr(tr('machines.set.imp.err.json')); }
  };
  const cols = 5;
  const rows = [];
  for (let i = 0; i < list.length; i += cols) rows.push(list.slice(i, i + cols));
  return <>
    <Head title={tr('machines.set.nav.profiles')} text={tr('machines.set.prof.text')}>
      <Btn label={tr('machines.set.prof.import')} onClick={() => setPicking(true)} />
      <Btn label={tr('machines.set.prof.duplicate')} onClick={() => fresh(sel)} />
      <Btn primary label={tr('machines.set.prof.new')} onClick={() => fresh(null)} />
    </Head>
    {err ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{err}</Text> : null}
    {rows.map((r, i) => (
      <View key={i} style={{ flexDirection: 'row', gap: 12 }}>
        {r.map((pr) => (
          <View key={pr.id} onClick={() => setSelId(pr.id)} onDoubleClick={() => (pr.builtin ? null : setEdit({ profile: pr }))}
            style={{ flex: 1, minWidth: 0, padding: 14, borderRadius: 14, backgroundColor: pr.id === sel.id ? (p.dark ? '#2a1820' : p.raised) : p.card,
              border: pr.id === sel.id ? `1.5px solid ${p.accent}` : `1px solid ${p.line}` }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{profileTitle(pr)}</Text>
            <Text style={{ marginTop: 4, fontSize: 12, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{profileHint(pr, def)}</Text>
          </View>
        ))}
        {Array.from({ length: cols - r.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />)}
      </View>
    ))}
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignItems: 'center' }}>
      <Text style={{ flex: 1, fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{profileTitle(sel)}</Text>
      {sel.id !== def ? <Btn label={tr('machines.set.prof.makeDefault')} onClick={() => setDefaults({ profile: sel.id })} /> : null}
      {!sel.builtin ? <>
        <Btn label={tr('machines.set.prof.edit')} onClick={() => setEdit({ profile: sel })} />
        <Btn danger label={tr('machines.set.prof.delete')} onClick={() => setDel(sel)} />
      </> : null}
    </View>
    <ProfileValues pr={sel} host={host} gpus={gpus} />
    <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.set.prof.fits')}</Text>
    {edit ? <Sheet width={1040} height={720} modal onClose={() => setEdit(null)}>
      <ProfileEdit host={host} gpus={gpus} start={edit.profile} fresh={edit.fresh} taken={own.filter((x) => x.id !== edit.profile.id).map((x) => x.name.toLowerCase())}
        onClose={() => setEdit(null)} onSaved={(pr) => { setEdit(null); setOwn(loadProfiles()); setSelId(pr.id); }} />
    </Sheet> : null}
    {imp ? <Sheet width={760} height={700} modal onClose={() => setImp(null)}>
      <ImportSummary file={imp.file} r={imp.r} host={host} onCancel={() => setImp(null)} onImport={() => {
        const pr = imp.r.profile;
        saveProfile(pr);
        if (imp.r.image) saveSystems([...loadSystems(), { ...imp.r.image, added: nowIso() }]);
        setImp(null); setOwn(loadProfiles()); setSelId(pr.id);
      }} />
    </Sheet> : null}
    {del ? <ConfirmDialog title={tr('machines.set.prof.deleteTitle', { name: del.name })} detail={tr('machines.set.prof.deleteDetail')} okLabel={tr('machines.set.prof.delete')} cancelLabel={tr('machines.cancel')} destructive
      onOk={() => { deleteProfile(del.id); setDel(null); setOwn(loadProfiles()); setSelId(DEFAULT_PROFILE); }} onCancel={() => setDel(null)} /> : null}
    {picking ? <FilePicker title={tr('machines.set.prof.importPick')} filter={/\.lordprofile$/i} onClose={() => setPicking(false)}
      onPick={(x) => { setPicking(false); const pth = Array.isArray(x) ? x[0] : x; if (pth) doImport(pth); }} /> : null}
  </>;
}

// ---- New profile / Edit profile (the mock "ProfileEdit"): six parts, "On this computer" beside them ----

const PARTS = ['identity', 'system', 'resources', 'network', 'integration', 'input'];
function Field({ label, children }) {
  const p = usePal();
  return (
    <View style={{ gap: 6 }}>
      <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{label}</Text>
      {children}
    </View>
  );
}
function Swatches({ value, onChange }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', gap: 10 }}>
      {CARD_COLORS.map((c) => (
        <View key={c} title={tr(`machines.set.color.${c}`)} onClick={() => onChange(c)} style={{ width: 28, height: 28, borderRadius: 14,
          background: `linear-gradient(135deg, ${COLOR_HEX[c][0]}, ${COLOR_HEX[c][1]})`, border: value === c ? `2px solid ${p.text}` : '2px solid transparent' }} />
      ))}
    </View>
  );
}
export function onHostLine(pr, host, gpuReady) {
  const v = onHost(pr, host, { gpuReady, no3d: no3dReason(host, false, host.arch) });
  return tr('machines.set.edit.wouldGive', { spec: `${tr('machines.lib.spec.cpu', { n: v.vcpus })} · ${gb(v.memoryMb)} · ${tr('machines.lib.spec.disk', { n: v.diskGb })}`,
    net: tr(`machines.cfg.net.${v.network}`).toLowerCase(), gfx: v.graphics === '3d' ? '3D' : '2D' });
}

function ProfileEdit({ host, gpus, start, fresh, taken, onClose, onSaved }) {
  const p = usePal();
  const [pr, setPr] = useState(() => JSON.parse(JSON.stringify(start)));
  const [pick, setPick] = useState(null);           // 'share' | 'export'
  const [msg, setMsg] = useState(null);
  const scroll = useRef(null);
  const refs = useRef({});
  const inner = useRef(null);
  const gReady = !!gvtGpu(gpus);
  const systems = loadSystems();
  const set = (part, patch) => setPr((x) => ({ ...x, [part]: { ...x[part], ...patch } }));
  const cap = cpuCap(host), mcap = memCap(host);
  const cpusNum = pr.resources.cpus === 'half' ? vcpuDefault(host, cap) : Math.min(cap, pr.resources.cpus);
  const issues = usesPorts(pr.network.mode) ? portProblems(pr.network.ports, pr.network.mode) : [];
  const nameBad = !pr.name.trim() ? 'empty' : taken.includes(pr.name.trim().toLowerCase()) ? 'taken' : '';
  const save = () => {
    if (nameBad || issues.length) { setMsg({ ok: false, text: nameBad ? tr(`machines.set.edit.name.${nameBad}`) : tr('machines.set.edit.portsBad') }); return; }
    const r = readProfile(writeProfile({ ...pr, name: pr.name.trim() }, [], { withId: true }), { stored: true });
    if (!r.ok) { setMsg({ ok: false, text: tr(`machines.set.imp.err.${r.error}`) }); return; }
    try { saveProfile(r.profile); onSaved(r.profile); } catch (e) { setMsg({ ok: false, text: e.message }); }
  };
  const jump = (k) => {
    const a = refs.current[k], top = inner.current;
    if (!a || !top || !scroll.current) return;
    scroll.current.scrollTo(lord.getLayout(a.id)[1] - lord.getLayout(top.id)[1]);
  };
  const catalog = systemsFor(CATALOG, host.arch).filter((s) => s.family === pr.system.kind);
  const mine = systems.filter((s) => s.kind === pr.system.kind);
  const toggleSys = (id) => set('system', { suggested: pr.system.suggested.includes(id) ? pr.system.suggested.filter((x) => x !== id) : [...pr.system.suggested, id].slice(0, 8) });
  const sysChip = (id, label) => {
    const on = pr.system.suggested.includes(id);
    return <View key={id} onClick={() => toggleSys(id)} style={{ height: 30, paddingHorizontal: 12, borderRadius: 15, justifyContent: 'center',
      backgroundColor: on ? p.accent : 'transparent', border: on ? 'none' : `1px solid ${p.line}` }}>
      <Text style={{ fontSize: 13, color: on ? '#fff' : p.text }}>{on ? `${label} ✓` : label}</Text></View>;
  };
  const section = (k, children) => (
    <View key={k} ref={(r) => { refs.current[k] = r; }} style={{ padding: 18, gap: 14, borderRadius: 14, backgroundColor: p.card, border: `1px solid ${p.line}` }}>
      <Text style={{ fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(`machines.set.edit.part.${k}`, { n: PARTS.indexOf(k) + 1 })}</Text>
      {children}
    </View>
  );
  return (
    <View style={{ flex: 1 }}>
      <SheetHeader title={`${tr('machines.set.title')} — ${tr(fresh ? 'machines.set.prof.new' : 'machines.set.prof.editTitle')}`}
        right={<Round title={tr('machines.cancel')} onClick={onClose}><Text style={{ fontSize: 15, color: p.dim }}>✕</Text></Round>} />
      <View style={{ flex: 1, minHeight: 0, flexDirection: 'row' }}>
        <View style={{ width: 230, paddingHorizontal: 10, paddingVertical: 16, gap: 2, borderEndWidth: 1, borderColor: p.line }}>
          {PARTS.map((k) => (
            <View key={k} onClick={() => jump(k)} style={{ paddingHorizontal: 12, paddingVertical: 9, borderRadius: 10 }}>
              <Text style={{ fontSize: 14, color: p.text, textAlign: 'start' }}>{tr(`machines.set.edit.part.${k}`, { n: PARTS.indexOf(k) + 1 })}</Text>
            </View>
          ))}
          <View style={{ height: 1, marginVertical: 12, backgroundColor: p.line }} />
          <View style={{ padding: 12, gap: 6, borderRadius: 12, backgroundColor: p.raised }}>
            <Text style={{ fontSize: 12, fontWeight: 700, color: p.dim, textAlign: 'start' }}>{tr('machines.set.edit.onThis')}</Text>
            <Text style={{ fontSize: 13, lineHeight: 1.45, color: p.text, textAlign: 'start' }}>{onHostLine(pr, host, gReady)}</Text>
          </View>
        </View>
        <View style={{ flex: 1, minWidth: 0 }}>
          <ScrollView scrollRef={scroll} style={{ flex: 1 }} contentStyle={{ padding: 24, gap: 16 }}>
            <View ref={inner} />
            {section('identity', <>
              <Field label={tr('machines.set.edit.name')}><TextInput value={pr.name} onChange={(v) => setPr((x) => ({ ...x, name: v.slice(0, 64) }))} autoFocus /></Field>
              <Field label={tr('machines.set.edit.author')}><TextInput value={pr.author} onChange={(v) => setPr((x) => ({ ...x, author: v.slice(0, 64) }))} /></Field>
              <Field label={tr('machines.set.edit.description')}><TextInput value={pr.description} onChange={(v) => setPr((x) => ({ ...x, description: v.slice(0, 280) }))} /></Field>
              <Field label={tr('machines.set.edit.color')}><Swatches value={pr.color} onChange={(c) => setPr((x) => ({ ...x, color: c }))} /></Field>
            </>)}
            {section('system', <>
              <Field label={tr('machines.set.edit.kind')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.system.kind} onChange={(v) => set('system', { kind: v })} options={SYSTEM_KINDS.map((k) => [k, kindText(k)])} /></View>
              </Field>
              <Field label={tr('machines.set.edit.suggested')}>
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
                  {catalog.map((s) => sysChip(s.id, [s.name, s.release].filter(Boolean).join(' ')))}
                  {mine.map((s) => sysChip(s.id, tr('machines.set.edit.yours', { name: s.name })))}
                </View>
              </Field>
              <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.edit.systemNote')}</Text>
            </>)}
            {section('resources', <>
              <Field label={tr('machines.set.prof.cpus')}>
                <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, direction: 'ltr' }}>
                  <Slider value={cpusNum} min={1} max={Math.max(1, cap)} step={1} width={260} onChange={(v) => set('resources', { cpus: Math.max(1, Math.round(v)) })} />
                  <Text style={{ fontSize: 13, fontWeight: 600, color: p.text }}>{pr.resources.cpus === 'half' ? tr('machines.set.edit.halfPc') : tr('machines.lib.spec.cpu', { n: cpusNum })}</Text>
                </View>
              </Field>
              <Field label={tr('machines.as.r.memory')}>
                <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, direction: 'ltr' }}>
                  <Slider value={Math.min(pr.resources.memoryMb, mcap)} min={512} max={Math.max(1024, mcap)} step={512} width={260} onChange={(v) => set('resources', { memoryMb: Math.max(512, Math.round(v / 512) * 512) })} />
                  <Text style={{ fontSize: 13, fontWeight: 600, color: p.text }}>{gb(Math.min(pr.resources.memoryMb, mcap))}</Text>
                </View>
              </Field>
              <Field label={tr('machines.as.r.disk')}>
                <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, direction: 'ltr' }}>
                  <Slider value={pr.resources.diskGb} min={8} max={512} step={8} width={260} onChange={(v) => set('resources', { diskGb: Math.max(8, Math.round(v / 8) * 8) })} />
                  <Text style={{ fontSize: 13, fontWeight: 600, color: p.text }}>{tr('machines.set.edit.diskGrows', { n: pr.resources.diskGb })}</Text>
                </View>
              </Field>
              <Field label={tr('machines.cfg.graphics')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.resources.graphics} onChange={(v) => set('resources', { graphics: v })} options={[['2d', '2D'], ['3d', '3D']]} /></View>
              </Field>
              <Field label={tr('machines.set.prof.gpu')}>
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 10 }}>
                  <View style={{ opacity: gReady ? 1 : 0.45, pointerEvents: gReady ? 'auto' : 'none' }}>
                    <Segmented small value={pr.resources.gpu} onChange={(v) => set('resources', { gpu: v })} options={GPU_CHOICES.map((k) => [k, gpuText(k)])} />
                  </View>
                  <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.set.edit.gpuOnly')}</Text>
                </View>
              </Field>
            </>)}
            {section('network', <>
              <Field label={tr('machines.cfg.netMode')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.network.mode} onChange={(v) => set('network', { mode: v, ports: usesPorts(v) ? pr.network.ports : [] })}
                  options={NET_MODES.map((k) => [k, tr(`machines.cfg.net.${k}`)])} /></View>
              </Field>
              {usesPorts(pr.network.mode) ? <Field label={tr('machines.cfg.ports')}>
                {pr.network.ports.map((r, i) => {
                  const bad = issues.find((x) => x.i === i);
                  const setRule = (patch) => set('network', { ports: pr.network.ports.map((x, k) => (k === i ? { ...x, ...patch } : x)) });
                  return (
                    <View key={i} style={{ gap: 4 }}>
                      <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 8, direction: 'ltr' }}>
                        <Segmented small value={r.proto} onChange={(x) => setRule({ proto: x })} options={[['tcp', 'TCP'], ['udp', 'UDP']]} />
                        <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.cfg.portHost')}</Text>
                        <TextInput value={r.host ? String(r.host) : ''} onChange={(x) => setRule({ host: portInput(x) })} style={{ width: 70 }} />
                        <Text style={{ fontSize: 12, color: p.dim }}>→ {tr('machines.cfg.portGuest')}</Text>
                        <TextInput value={r.guest ? String(r.guest) : ''} onChange={(x) => setRule({ guest: portInput(x) })} style={{ width: 70 }} />
                        {pr.network.mode === 'shared' ? <Segmented small value={r.open === 'lan' ? 'lan' : 'this-computer'} onChange={(x) => setRule({ open: x })}
                          options={[['this-computer', tr('machines.cfg.portOpen.local')], ['lan', tr('machines.cfg.portOpen.lan')]]} /> : null}
                        <Btn label={tr('machines.cfg.portRemove')} onClick={() => set('network', { ports: pr.network.ports.filter((_, k) => k !== i) })} />
                      </View>
                      {r.open === 'lan' && pr.network.mode === 'shared' ? <Text style={{ fontSize: 12, color: '#ff9f0a', textAlign: 'start' }}>{tr('machines.set.edit.lanWarn')}</Text> : null}
                      {bad ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.cfg.portProblem.${bad.code}`, { min: PORT_HOST_MIN })}</Text> : null}
                    </View>
                  );
                })}
                <View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.set.edit.addPort')} onClick={() => set('network', { ports: [...pr.network.ports, newPortRule(pr.network.ports)] })} /></View>
              </Field> : null}
            </>)}
            {section('integration', <>
              <Field label={tr('machines.set.prof.clipboard')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.integration.clipboard} onChange={(v) => set('integration', { clipboard: v })} options={CLIPBOARDS.map((k) => [k, tr(`machines.cfg.clip.${k}`)])} /></View>
              </Field>
              <Field label={tr('machines.set.edit.share')}>
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 10 }}>
                  <Text style={{ fontSize: 13, fontWeight: 600, color: p.text, direction: 'ltr' }}>{pr.integration.share ? pr.integration.share.path : tr('machines.set.edit.shareNone')}</Text>
                  {pr.integration.share ? <>
                    <Toggle small on={pr.integration.share.writable} onChange={(w) => set('integration', { share: { ...pr.integration.share, writable: w } })} />
                    <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.set.edit.shareWritable')}</Text>
                  </> : null}
                  <Btn label={tr('machines.set.edit.shareChoose')} onClick={() => setPick('share')} />
                  {pr.integration.share ? <Btn label={tr('machines.set.edit.shareRemove')} onClick={() => set('integration', { share: null })} /> : null}
                </View>
              </Field>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
                <Toggle on={pr.integration.transfer} onChange={(v) => set('integration', { transfer: v })} />
                <Text style={{ fontSize: 13, color: p.text }}>{tr('machines.cfg.transfer')}</Text>
              </View>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
                <Toggle on={pr.integration.appsMode} onChange={(v) => set('integration', { appsMode: v })} />
                <Text style={{ fontSize: 13, color: p.text }}>{tr('machines.set.prof.appsMode')}</Text>
              </View>
              <Field label={tr('machines.set.gen.onClose')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.integration.onClose} onChange={(v) => set('integration', { onClose: v })} options={CLOSE_CHOICES.map((k) => [k, tr(`machines.cfg.close.${k}`)])} /></View>
              </Field>
            </>)}
            {section('input', <>
              <Field label={tr('machines.set.edit.mouse')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.input.mouse} onChange={(v) => set('input', { mouse: v })} options={MOUSE.map((k) => [k, tr(`machines.set.edit.mouse.${k}`)])} /></View>
              </Field>
              <Field label={tr('machines.set.edit.shortcuts')}>
                <View style={{ alignSelf: 'flex-start' }}><Segmented small value={pr.input.shortcuts} onChange={(v) => set('input', { shortcuts: v })} options={SHORTCUTS.map((k) => [k, tr(`machines.set.edit.shortcuts.${k}`)])} /></View>
              </Field>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
                <Toggle on={pr.input.captureFullscreen} onChange={(v) => set('input', { captureFullscreen: v })} />
                <Text style={{ fontSize: 13, color: p.text }}>{tr('machines.set.edit.captureFull')}</Text>
              </View>
            </>)}
          </ScrollView>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingVertical: 14, borderTopWidth: 1, borderColor: p.line }}>
            <Pill label={tr('machines.set.edit.export')} disabled={!!nameBad} onClick={() => setPick('export')} />
            {msg ? <Text style={{ flex: 1, fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : <View style={{ flex: 1 }} />}
            <Pill label={tr('machines.cancel')} onClick={onClose} />
            <Pill primary label={tr('machines.set.edit.save')} onClick={save} />
          </View>
        </View>
      </View>
      {pick === 'share' ? <FilePicker mode="folder" title={tr('machines.set.edit.shareChoose')} onClose={() => setPick(null)} onPick={(x) => {
        setPick(null);
        const rel = shareRel(Array.isArray(x) ? x[0] : x, fs.home());
        if (rel) set('integration', { share: { path: rel, writable: pr.integration.share?.writable || false } }); else setMsg({ ok: false, text: tr('machines.set.edit.shareBad') });
      }} /> : null}
      {pick === 'export' ? <FilePicker mode="save" name={exportName(pr)} title={tr('machines.set.edit.export')} onClose={() => setPick(null)} onPick={(x) => {
        setPick(null);
        const out = Array.isArray(x) ? x[0] : x;
        if (!out) return;
        try { fs.write(/\.lordprofile$/i.test(out) ? out : `${out}.lordprofile`, writeProfile({ ...pr, name: pr.name.trim() }, systems)); setMsg({ ok: true, text: tr('machines.set.edit.exported', { file: out.split('/').pop() }) }); }
        catch (e) { setMsg({ ok: false, text: e.message }); }
      }} /> : null}
    </View>
  );
}

// ---- Import a profile (the mock "ProfileImport"): everything it brings, what was fitted or ignored, before it's kept ----

function adjustedText(a) {
  const field = String(a.field).split('.').pop();
  return tr(`machines.set.imp.adj.${a.why}`, { field, from: typeof a.from === 'object' ? JSON.stringify(a.from) : String(a.from), to: String(a.to ?? '') });
}
// every rule with who can reach it (after import: this computer only)
function portsText(ports) { return ports.map((r) => tr(r.open === 'lan' ? 'machines.set.imp.portLan' : 'machines.set.imp.portLocal', { rule: `${r.proto.toUpperCase()} ${r.host} → ${r.guest}` })).join(', '); }

function ImportSummary({ file, r, host, onCancel, onImport }) {
  const p = usePal();
  const pr = r.profile, im = r.image;
  const v = onHost(pr, host, { gpuReady: true, no3d: '' });
  const sugg = pr.system.suggested.map((id) => (im && id === im.id ? tr('machines.set.imp.newSys', { name: im.name }) : systemsFor(CATALOG, host.arch).find((s) => s.id === id)?.name || id)).join(', ');
  const kv = (k, val, mono) => (
    <View key={k} style={{ flexDirection: 'row', gap: 12, paddingVertical: 6 }}>
      <Text style={{ width: 170, fontSize: 13, color: p.dim, textAlign: 'start' }}>{k}</Text>
      <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start', direction: mono ? 'ltr' : undefined, fontFamily: mono ? 'monospace' : undefined, overflowWrap: 'anywhere' }}>{val}</Text>
    </View>
  );
  const notes = [
    ...r.adjusted.map(adjustedText),
    ...r.ignored.map((f) => tr('machines.set.imp.ignored', { field: f })),
    ...(r.imageErrors.length ? [tr('machines.set.imp.imageRefused')] : []),
  ];
  const logo = pr.logo || im?.logo;
  return (
    <View style={{ flex: 1 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14, paddingHorizontal: 22, paddingVertical: 18, borderBottomWidth: 1, borderColor: p.line }}>
        <View style={{ width: 44, height: 44, borderRadius: 10, background: `linear-gradient(135deg, ${COLOR_HEX[pr.color][0]}, ${COLOR_HEX[pr.color][1]})` }} />
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 18, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.set.imp.title', { name: pr.name })}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{[file, pr.author ? tr('machines.set.imp.by', { name: pr.author }) : null, tr('machines.set.imp.format', { n: 1 })].filter(Boolean).join(' · ')}</Text>
        </View>
        <View style={{ height: 24, paddingHorizontal: 10, borderRadius: 12, justifyContent: 'center', backgroundColor: 'rgba(48,209,88,0.18)' }}>
          <Text style={{ fontSize: 12, fontWeight: 700, color: '#30d158' }}>{tr('machines.set.imp.valid')}</Text>
        </View>
      </View>
      <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: 22, paddingVertical: 16, gap: 14 }}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr(im ? 'machines.set.imp.introSystem' : 'machines.set.imp.intro')}</Text>
        {im ? (
          <View style={{ padding: 14, gap: 4, borderRadius: 12, backgroundColor: p.raised, border: `1px solid ${p.line}` }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, marginBottom: 6 }}>
              <OsMark id="other" color={im.color} letter={im.name} size={36} />
              <View style={{ flex: 1 }}>
                <Text style={{ fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.set.imp.newSys', { name: im.name })}</Text>
                <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{[kindText(im.kind), tr(`machines.set.sys.install.${im.install}`), im.size ? bytes(im.size) : null].filter(Boolean).join(' · ')}</Text>
              </View>
            </View>
            {kv(tr('machines.set.imp.from'), im.url, true)}
            {kv(tr('machines.set.imp.sha'), im.sha256, true)}
            {kv(tr('machines.set.imp.signature'), im.signature ? `${fileOfUrl(im.signature.url) || hostOf(im.signature.url)} · ${tr('machines.set.imp.key', { fpr: fprText(im.signature.fingerprint) })} — ${tr(im.signature.key ? 'machines.set.sys.sigWithKey' : 'machines.set.sys.sigNotChecked')}` : tr('machines.set.imp.none'), !!im.signature)}
            <Text style={{ marginTop: 6, fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.imp.httpsOnly')}</Text>
          </View>
        ) : null}
        <View>
          {kv(tr('machines.set.edit.suggested'), sugg || tr('machines.set.imp.none'))}
          {kv(tr('machines.set.prof.resources'), `${tr('machines.lib.spec.cpu', { n: v.vcpus })} · ${gb(v.memoryMb)} · ${tr('machines.lib.spec.disk', { n: v.diskGb })} · ${pr.resources.graphics === '3d' ? '3D' : '2D'}`)}
          {kv(tr('machines.as.r.network'), [tr(`machines.cfg.net.${pr.network.mode}`), pr.network.ports.length ? tr('machines.set.imp.ports', { list: portsText(pr.network.ports) }) : null].filter(Boolean).join(' · '))}
          {kv(tr('machines.set.prof.clipboard'), tr(`machines.cfg.clip.${pr.integration.clipboard}`))}
          {kv(tr('machines.set.edit.share'), pr.integration.share ? tr('machines.set.imp.share', { path: pr.integration.share.path, how: tr(pr.integration.share.writable ? 'machines.set.imp.write' : 'machines.set.imp.read') }) : tr('machines.set.edit.shareNone'))}
          {logo ? kv(tr('machines.set.imp.logo'), `${logo.url} — ${tr('machines.set.imp.logoNotDownloaded')}`, true) : null}
        </View>
        {exposureOf(pr).length ? (
          <View style={{ padding: 14, gap: 6, borderRadius: 12, backgroundColor: p.raised, border: `1px solid ${p.line}` }}>
            <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.set.imp.exposure')}</Text>
            {exposureOf(pr).map((k) => <Text key={k} style={{ fontSize: 12, lineHeight: 1.4, color: p.text, textAlign: 'start' }}>{tr(`machines.set.imp.${k}`)}</Text>)}
          </View>
        ) : null}
        {notes.length ? (
          <View style={{ padding: 14, gap: 6, borderRadius: 12, backgroundColor: p.warnBg }}>
            <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.set.imp.adjusted')}</Text>
            {notes.map((t0, i) => <Text key={i} style={{ fontSize: 12, lineHeight: 1.4, color: p.text, textAlign: 'start' }}>{t0}</Text>)}
          </View>
        ) : null}
        {logo ? <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.imp.logoTheirs')}</Text> : null}
      </ScrollView>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingVertical: 14, borderTopWidth: 1, borderColor: p.line }}>
        <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.set.imp.stays')}</Text>
        <Pill label={tr('machines.cancel')} onClick={onCancel} />
        <Pill primary label={tr('machines.set.imp.ok')} onClick={onImport} />
      </View>
    </View>
  );
}

// ---- Systems (the mock "Systems") ----

function SysRow({ mark, title, sub, right }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14, paddingHorizontal: 16, paddingVertical: 12, borderBottomWidth: 1, borderColor: p.line }}>
      {mark}
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 14, fontWeight: 600, color: p.text, textAlign: 'start' }}>{title}</Text>
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{sub}</Text>
      </View>
      {right}
    </View>
  );
}

function Systems({ host }) {
  const p = usePal();
  // the evaluation of the other system in the language of this computer, as the assistant picks it
  const locale = (has() && lord.vm.hostDefaults?.()?.locale) || 'en_US.UTF-8';
  const rev = useStore(storeRev);
  const [mine, setMine] = useState(loadSystems);
  useEffect(() => { setMine(loadSystems()); }, [rev]);
  const [have, setHave] = useState(downloadedIds);
  const [dl, setDl] = useState(null);                // { id, got: { done, total }, err }
  const [edit, setEdit] = useState(null);            // a system (new: { fresh: true })
  const [get, setGet] = useState(null);              // a system whose download the person confirms
  const [udl, setUdl] = useState(null);              // a system's download: { id, got: { done, total }, phase: 'sig', err }
  const [watch, setWatch] = useState(null);          // a file the person chose, hashed: { id, phase: 'hashing', job, path, pct, err }
  const [pickFor, setPickFor] = useState(null);
  const own = loadProfiles();
  const cards = systemsFor(CATALOG, host.arch);
  const startDl = async (s) => {
    setDl({ id: s.id, got: { done: 0, total: 0 } });
    try {
      await downloadSystem(s.id, host.arch, locale, (got) => setDl((d) => (d && d.id === s.id ? { ...d, got } : d)));
      setDl(null); setHave(downloadedIds());
    } catch (e) { setDl({ id: s.id, err: e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed') }); }
  };
  // an image the person already downloaded and chose: hashed by lord-vm, kept only if it matches
  const check = (s, path) => {
    const job = lord.vm.hash(path);
    if (!job) { setWatch({ id: s.id, err: tr('machines.win.cantRead', { file: path.split('/').pop() }) }); return; }
    setWatch({ id: s.id, phase: 'hashing', job, path, pct: 0 });
  };
  // the image from the system's address, by the admin helper (vm-image-get-user: kept only with its SHA-256), then its
  // signature by lord-pgpv when the system has a key: verified only when both hold; the key's fingerprint is pinned
  // for this system on its first good check
  const getUser = async (s) => {
    setUdl({ id: s.id, got: { done: 0, total: s.size || 0 } });
    try {
      const path = await downloadUserSystem(s, (got) => setUdl((d) => (d && d.id === s.id && !d.err ? { ...d, got } : d)));
      let sigOk = null;
      if (signatureState(s.signature) === 'check') {
        setUdl({ id: s.id, phase: 'sig' });
        const v = await checkSignature(s);
        if (!v.valid) { setUdl({ id: s.id, err: tr(v.reason === 'notListed' ? 'machines.set.sys.sigNotListed' : 'machines.set.sys.sigBad') }); return; }
        sigOk = { fingerprint: v.fingerprint, at: nowIso() };
      }
      saveSystems(loadSystems().map((x) => (x.id === s.id ? { ...x, verified: { path, at: nowIso() },
        ...(sigOk && x.signature ? { sigOk, signature: { ...x.signature, pinned: x.signature.pinned || sigOk.fingerprint } } : {}) } : x)));
      setUdl(null);
    } catch (e) { setUdl({ id: s.id, err: e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed') }); }
  };
  const trustKey = (s) => saveSystems(loadSystems().map((x) => (x.id === s.id && x.signature ? { ...x, signature: { ...x.signature, pinned: undefined }, sigOk: undefined } : x)));
  useEffect(() => {
    if (!watch) return undefined;
    const s = mine.find((x) => x.id === watch.id);
    if (!s) return undefined;
    if (watch.phase !== 'hashing') return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(watch.job);
      if (!r) return;
      let pct = 0, sha = '';
      for (const l of String(r.out || '').trim().split('\n')) { try { const o = JSON.parse(l); if (typeof o.progress === 'number') pct = o.progress; if (o.sha256) sha = o.sha256; } catch (e) { /* partial */ } }
      if (!r.done) { setWatch((w) => (w && w.job === watch.job ? { ...w, pct } : w)); return; }
      if (r.ok && sha && sha.toLowerCase() === s.sha256) {
        saveSystems(loadSystems().map((x) => (x.id === s.id ? { ...x, verified: { path: watch.path, at: nowIso() } } : x)));
        setWatch(null);
      } else setWatch({ id: s.id, err: tr('machines.set.sys.shaMismatch', { file: watch.path.split('/').pop() }) });
    }, 400);
    return () => clearInterval(timer);
  }, [watch?.id, watch?.phase, watch?.job, mine]);
  const profName = (id) => { const x = allProfiles(own, host).find((y) => y.id === id); return x ? profileTitle(x) : profileTitle(builtinProfile(DEFAULT_PROFILE, host)); };
  const subOf = (s) => [tr(s.source === 'file' ? 'machines.set.sys.fromIso' : 'machines.set.sys.fromUrl', { host: hostOf(s.url) }),
    s.verified ? tr('machines.set.sys.verified') : tr('machines.set.sys.notVerified'), tr(`machines.set.sys.install.${s.install}`),
    s.sigOk ? tr('machines.set.sys.sigOk', { fpr: fprText(s.sigOk.fingerprint) }) : s.signature ? tr('machines.set.sys.sigNotChecked') : null].filter(Boolean).join(' · ');
  return <>
    <Head title={tr('machines.set.nav.systems')} text={tr('machines.set.sys.text')}>
      <Btn primary label={tr('machines.set.sys.add')} onClick={() => setEdit({ fresh: true })} />
    </Head>
    <Section title={tr('machines.set.sys.catalog')}>
      {cards.map((s) => {
        const busy = dl?.id === s.id && !dl.err;
        const sub = s.kind === 'windows' ? tr('machines.win.sys.catalogHint') : unattendedOf(CATALOG, s.id, host.arch) ? tr('machines.set.sys.unattended') : tr('machines.set.sys.manual');
        return <SysRow key={s.id} mark={<OsMark id={s.id} size={36} />} title={[s.name, s.release].filter(Boolean).join(' ')}
          sub={dl?.id === s.id && dl.err ? dl.err : busy && dl.got?.total ? tr('machines.dl.progress', { a: bytes(dl.got.done), b: bytes(dl.got.total) }) : sub}
          right={have.has(s.id) ? <Text style={{ fontSize: 13, fontWeight: 600, color: '#30d158' }}>{tr('machines.set.sys.downloaded')}</Text>
            : busy ? <Btn label={tr('machines.dl.cancel')} onClick={() => { stopDownload(); setDl(null); }} />
            : <Btn label={tr('machines.set.sys.download', { size: bytes(s.approxMb * 1048576) })} disabled={!!dl && !dl.err} onClick={() => startDl(s)} />} />;
      })}
    </Section>
    <Section title={tr('machines.set.sys.yours')}>
      {mine.map((s) => {
        const st = systemState(s);
        const w = watch?.id === s.id ? watch : null;
        const u = udl?.id === s.id ? udl : null;
        const busy = !!u && !u.err;
        const changed = signatureState(s.signature) === 'keyChanged';
        const sub = u?.err ? u.err : u?.phase === 'sig' ? tr('machines.set.sys.sigChecking') : busy ? (u.got?.total ? tr('machines.dl.progress', { a: bytes(u.got.done), b: bytes(u.got.total) }) : tr('machines.dl.downloading', { name: s.name }))
          : w?.err ? w.err : w?.phase === 'hashing' ? tr('machines.set.sys.hashing', { pct: w.pct || 0 })
          : changed && st === 'download' ? tr('machines.set.sys.keyChanged', { fpr: fprText(s.signature.pinned) }) : subOf(s);
        return <SysRow key={s.id} mark={<OsMark id="other" color={s.color} letter={s.name} size={36} />} title={s.name}
          sub={sub}
          right={<View style={{ flexDirection: 'row', gap: 8 }}>
            {busy && u.phase !== 'sig' ? <Btn label={tr('machines.dl.cancel')} onClick={() => { stopDownload(); setUdl(null); }} /> : null}
            {changed && !busy ? <Btn label={tr('machines.set.sys.keyTrust')} onClick={() => trustKey(s)} /> : null}
            {st === 'download' && !busy && !w?.phase && !changed ? <Btn label={tr('machines.set.sys.downloadUrl')} disabled={!!udl && !udl.err} onClick={() => setGet(s)} /> : null}
            {st !== 'ready' && !busy ? <Btn label={tr('machines.set.sys.chooseFile')} onClick={() => setPickFor(s)} /> : null}
            <Btn label={tr('machines.set.sys.edit')} onClick={() => setEdit(s)} />
          </View>} />;
      })}
      <View style={{ paddingHorizontal: 16, paddingVertical: 12 }}><Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.yoursHint')}</Text></View>
    </Section>
    {get ? <Sheet width={680} height={480} modal onClose={() => setGet(null)}>
      <View style={{ flex: 1 }}>
        <SheetHeader title={tr('machines.set.sys.getTitle', { name: get.name })} />
        <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 22, gap: 10 }}>
          <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.set.sys.getText')}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.set.imp.from')}</Text>
          <Text style={{ fontSize: 13, color: p.text, textAlign: 'start', direction: 'ltr', fontFamily: 'monospace', overflowWrap: 'anywhere' }}>{get.url}</Text>
          {get.size ? <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{bytes(get.size)}</Text> : null}
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.set.imp.sha')}</Text>
          <Text style={{ fontSize: 13, color: p.text, textAlign: 'start', direction: 'ltr', fontFamily: 'monospace', overflowWrap: 'anywhere' }}>{get.sha256}</Text>
          {get.signature ? <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{signatureState(get.signature) === 'check'
            ? tr('machines.set.sys.sigWillCheck', { fpr: fprText(get.signature.fingerprint) }) : tr('machines.set.sys.sigNotChecked')}</Text> : null}
        </ScrollView>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 10, paddingHorizontal: 20, paddingVertical: 14, borderTopWidth: 1, borderColor: p.line }}>
          <Pill label={tr('machines.cancel')} onClick={() => setGet(null)} />
          <Pill primary label={tr('machines.set.sys.getOk')} onClick={() => { const s = get; setGet(null); getUser(s); }} />
        </View>
      </View>
    </Sheet> : null}
    {edit ? <Sheet width={760} height={700} modal onClose={() => setEdit(null)}>
      <SystemEdit start={edit.fresh ? null : edit} host={host} profiles={allProfiles(own, host)} taken={mine.map((x) => x.id)} onClose={() => setEdit(null)}
        onSave={(s) => { saveSystems(edit.fresh ? [...loadSystems(), s] : loadSystems().map((x) => (x.id === s.id ? s : x))); setEdit(null); }}
        onDelete={edit.fresh ? null : () => { saveSystems(loadSystems().filter((x) => x.id !== edit.id)); setEdit(null); }} />
    </Sheet> : null}
    {pickFor ? <FilePicker title={tr('machines.new.pickIso')} filter={/\.iso$/i} onClose={() => setPickFor(null)}
      onPick={(x) => { const s = pickFor; setPickFor(null); const pth = Array.isArray(x) ? x[0] : x; if (pth) check(s, pth); }} /> : null}
  </>;
}

// Add a system / Edit: name, kind, color, default profile, how it installs, and its image (an ISO here, hashed now, or an
// https address with the SHA-256 its maker publishes); optional signature fields and logo
function SystemEdit({ start, host, profiles, taken, onClose, onSave, onDelete }) {
  const p = usePal();
  const [s, setS] = useState(() => start ? { ...start, sig: { key: '', ...(start.signature || { url: '', sums: '', fingerprint: '' }) }, logo0: start.logo || { url: '', sha256: '' } }
    : { name: '', kind: 'linux', color: 'blue', profile: DEFAULT_PROFILE, install: 'manual', source: 'file', path: '', url: '', sha256: '', sig: { url: '', sums: '', fingerprint: '', key: '' }, logo0: { url: '', sha256: '' } });
  const [picking, setPicking] = useState(false);
  const [pickKey, setPickKey] = useState(false);
  const [hash, setHash] = useState(null);            // { job, pct }
  const [errs, setErrs] = useState([]);
  const put = (patch) => { setErrs([]); setS((x) => ({ ...x, ...patch })); };
  useEffect(() => {
    if (!hash) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(hash.job);
      if (!r) return;
      let pct = 0, sha = '';
      for (const l of String(r.out || '').trim().split('\n')) { try { const o = JSON.parse(l); if (typeof o.progress === 'number') pct = o.progress; if (o.sha256) sha = o.sha256; } catch (e) { /* partial */ } }
      if (!r.done) { setHash((h) => (h ? { ...h, pct } : h)); return; }
      setHash(null);
      if (r.ok && sha) put({ sha256: sha.toLowerCase(), verified: { path: s.path, at: nowIso() } }); else setErrs(['hash']);
    }, 400);
    return () => clearInterval(timer);
  }, [hash?.job]);
  const build = () => {
    const sig = s.sig.url || s.sig.sums || s.sig.fingerprint || s.sig.key ? s.sig : null;
    const logo = s.logo0.url || s.logo0.sha256 ? s.logo0 : null;
    const o = { id: start?.id, name: s.name, kind: s.kind, color: s.color, profile: s.profile, install: s.kind === 'linux' ? s.install : 'manual', source: s.source,
      ...(s.source === 'file' ? { path: s.path } : { url: s.url }), sha256: s.sha256, ...(s.size ? { size: s.size } : {}), signature: sig, logo, added: start?.added || nowIso(),
      ...(s.source === 'file' && s.verified ? { verified: s.verified } : start?.source === 'url' && s.url === start.url && s.sha256 === start.sha256 && start.verified ? { verified: start.verified } : {}),
      // a signature that verified stays only while its addresses, key and fingerprint are the same
      ...(start?.sigOk && start.signature && sig && sig.url === start.signature.url && sig.sums === start.signature.sums && sig.key === (start.signature.key || '')
        && sig.fingerprint === start.signature.fingerprint ? { sigOk: start.sigOk } : {}) };
    return validateSystem(o, { home: fs.home(), profiles: profiles.map((x) => x.id), newId: start?.id || newId('s', taken) });
  };
  const save = () => { const r = build(); if (!r.ok) { setErrs(r.errors); return; } onSave(r.system); };
  const field = (label, value, onChange, opts = {}) => (
    <Field label={label}><TextInput value={value} onChange={onChange} placeholder={opts.placeholder} style={opts.mono ? { direction: 'ltr' } : undefined} /></Field>
  );
  return (
    <View style={{ flex: 1 }}>
      <SheetHeader title={start ? tr('machines.set.sys.editTitle', { name: start.name }) : tr('machines.set.sys.add')}
        right={<Round title={tr('machines.cancel')} onClick={onClose}><Text style={{ fontSize: 15, color: p.dim }}>✕</Text></Round>} />
      <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 22, gap: 14 }}>
        {field(tr('machines.set.edit.name'), s.name, (v) => put({ name: v.slice(0, 64) }))}
        <Field label={tr('machines.set.edit.kind')}>
          <View style={{ alignSelf: 'flex-start' }}><Segmented small value={s.kind} onChange={(v) => put({ kind: v, install: v === 'linux' ? s.install : 'manual' })} options={SYSTEM_KINDS.map((k) => [k, kindText(k)])} /></View>
        </Field>
        <Field label={tr('machines.set.sys.icon')}><Swatches value={s.color} onChange={(c) => put({ color: c })} /></Field>
        <Field label={tr('machines.set.sys.profile')}>
          <View style={{ alignSelf: 'flex-start' }}><MenuBtn label={profileTitle(profiles.find((x) => x.id === s.profile) || profiles[0])}
            items={profiles.map((x) => ({ label: profileTitle(x), checked: x.id === s.profile, onClick: () => put({ profile: x.id }) }))} /></View>
        </Field>
        {s.kind === 'linux' ? <Field label={tr('machines.set.sys.installs')}>
          <View style={{ alignSelf: 'flex-start' }}><Segmented small value={s.install} onChange={(v) => put({ install: v })} options={INSTALLS.map((k) => [k, tr(`machines.set.sys.install.${k}`)])} /></View>
          <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.installNote')}</Text>
        </Field> : null}
        <Field label={tr('machines.set.sys.image')}>
          <View style={{ alignSelf: 'flex-start' }}><Segmented small value={s.source} onChange={(v) => put({ source: v, sha256: '', verified: undefined })}
            options={[['file', tr('machines.set.sys.srcFile')], ['url', tr('machines.set.sys.srcUrl')]]} /></View>
        </Field>
        {s.source === 'file' ? <>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Text style={{ flex: 1, fontSize: 13, color: s.path ? p.text : p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{s.path || tr('machines.set.sys.noFile')}</Text>
            <Btn label={tr('machines.set.sys.chooseIso')} onClick={() => setPicking(true)} />
          </View>
          {hash ? <View style={{ gap: 6 }}><Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.hashing', { pct: hash.pct || 0 })}</Text><Bar frac={(hash.pct || 0) / 100} color={p.accent} /></View>
            : s.sha256 ? <Text style={{ fontSize: 12, color: '#30d158', textAlign: 'start', direction: 'ltr' }}>{`SHA-256 ${s.sha256}`}</Text> : null}
        </> : <>
          {field(tr('machines.set.sys.url'), s.url, (v) => put({ url: v.trim() }), { placeholder: 'https://', mono: true })}
          {field(tr('machines.set.imp.sha'), s.sha256, (v) => put({ sha256: v.trim() }), { mono: true })}
          <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.urlNote')}</Text>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.set.sys.sigTitle')}</Text>
          {field(tr('machines.set.sys.sigUrl'), s.sig.url, (v) => put({ sig: { ...s.sig, url: v.trim() } }), { placeholder: 'https://', mono: true })}
          {field(tr('machines.set.sys.sumsUrl'), s.sig.sums, (v) => put({ sig: { ...s.sig, sums: v.trim() } }), { placeholder: 'https://', mono: true })}
          {field(tr('machines.set.sys.fpr'), s.sig.fingerprint, (v) => put({ sig: { ...s.sig, fingerprint: v } }), { mono: true })}
          <Field label={tr('machines.set.sys.key')}>
            <TextArea value={s.sig.key || ''} minRows={4} size={12} placeholder="-----BEGIN PGP PUBLIC KEY BLOCK-----" style={{ direction: 'ltr', fontFamily: 'monospace' }}
              onChange={(v) => put({ sig: { ...s.sig, key: v.slice(0, KEY_MAX + 200) } })} />
            <View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.set.sys.keyFile')} onClick={() => setPickKey(true)} /></View>
          </Field>
          <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.sigNote')}</Text>
        </>}
        <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.set.sys.logoTitle')}</Text>
        {field(tr('machines.set.sys.logoUrl'), s.logo0.url, (v) => put({ logo0: { ...s.logo0, url: v.trim() } }), { placeholder: 'https://….png', mono: true })}
        {field(tr('machines.set.imp.sha'), s.logo0.sha256, (v) => put({ logo0: { ...s.logo0, sha256: v.trim() } }), { mono: true })}
        <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.logoNote')}</Text>
        {errs.length ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{errs.map((e) => tr(`machines.set.sys.err.${e}`)).join(' ')}</Text> : null}
      </ScrollView>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingVertical: 14, borderTopWidth: 1, borderColor: p.line }}>
        {onDelete ? <Pill danger label={tr('machines.set.prof.delete')} onClick={onDelete} /> : null}
        <View style={{ flex: 1 }} />
        <Pill label={tr('machines.cancel')} onClick={onClose} />
        <Pill primary label={tr('machines.set.sys.save')} disabled={!!hash} onClick={save} />
      </View>
      {pickKey ? <FilePicker title={tr('machines.set.sys.keyFile')} filter={/\.(asc|txt|key|pub)$/i} onClose={() => setPickKey(false)} onPick={(x) => {
        setPickKey(false);
        const pth = Array.isArray(x) ? x[0] : x;
        let text = '';
        try { text = pth ? fs.read(pth) : ''; } catch (e) { text = ''; }
        const key = keyOf(typeof text === 'string' && text.length <= KEY_MAX + 200 ? text : '');
        if (key) put({ sig: { ...s.sig, key } }); else setErrs(['key']);
      }} /> : null}
      {picking ? <FilePicker title={tr('machines.new.pickIso')} filter={/\.iso$/i} onClose={() => setPicking(false)} onPick={(x) => {
        setPicking(false);
        const pth = Array.isArray(x) ? x[0] : x;
        if (!pth) return;
        put({ path: pth, sha256: '', verified: undefined, name: s.name || pth.split('/').pop().replace(/\.iso$/i, '') });
        const job = lord.vm.hash(pth);
        if (job) setHash({ job, pct: 0 }); else setErrs(['hash']);
      }} /> : null}
    </View>
  );
}

// ---- Network ----

function Network({ host }) {
  const p = usePal();
  const [s0] = useSettings();
  const d = defaultsOf(s0.machinesDefaults);
  const cards = (host.cards || []).filter((c) => c.kind === 'wired');
  const own = loadProfiles();
  const def = allProfiles(own, host).find((x) => x.id === defaultProfileId(d.profile, own)) || builtinProfile(DEFAULT_PROFILE, host);
  const cur = cards.find((c) => c.name === d.bridge);
  return <>
    <Head title={tr('machines.set.nav.network')} text={tr('machines.set.net.text')} />
    <Section>
      <Row label={tr('machines.set.net.default')} value={tr(`machines.cfg.net.${def.network.mode}`)} note={tr('machines.set.net.defaultNote', { profile: profileTitle(def) })} />
      <Row label={tr('machines.set.net.card')} last note={cards.length ? tr('machines.set.net.cardNote') : tr('machines.cfg.bridgeNoCards')}>
        {cards.length ? <View style={{ alignSelf: 'flex-start' }}><MenuBtn label={cur ? cur.name : tr('machines.set.net.cardAuto')}
          items={[{ label: tr('machines.set.net.cardAuto'), checked: !cur, onClick: () => setDefaults({ bridge: '' }) },
            ...cards.map((c) => ({ label: c.up ? c.name : tr('machines.cfg.bridgeCardDown', { name: c.name }), checked: c.name === d.bridge, onClick: () => setDefaults({ bridge: c.name }) }))]} /></View> : null}
      </Row>
    </Section>
    <Section title={tr('machines.set.net.modes')}>
      {NET_MODES.map((k, i) => <Row key={k} label={tr(`machines.cfg.net.${k}`)} note={tr(`machines.cfg.netNote.${k}`)} last={i === NET_MODES.length - 1} />)}
    </Section>
  </>;
}

// ---- Graphics cards: what Settings → Graphics cards set up ----

function Gpus({ gpus }) {
  const p = usePal();
  const list = gpus?.gpus || [];
  const hex4 = (v) => ((v >>> 0) & 0xffff).toString(16).padStart(4, '0');
  const name = (g) => g.name || (g.vendorName ? `${g.vendorName} (${hex4(g.vendor)}:${hex4(g.device)})` : `${hex4(g.vendor)}:${hex4(g.device)}`);
  const role = (g) => (g.desktop ? tr('machines.set.gpus.desktop') : g.gvtOn ? tr('machines.set.gpus.shared') : tr('machines.set.gpus.unused'));
  return <>
    <Head title={tr('machines.set.nav.gpus')} text={tr('machines.set.gpus.text')}>
      <Btn label={tr('machines.set.gpus.open')} onClick={() => wm.open('settings', { args: { section: 'gpus', nonce: Date.now() } })} />
    </Head>
    {list.length ? <Section>
      {list.map((g, i) => <Row key={g.address || i} label={name(g)} value={role(g)} last={i === list.length - 1} />)}
    </Section> : <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.set.gpus.none')}</Text>}
    {list.length === 1 ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.set.gpus.one')}</Text> : null}
    {gpus?.restart ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('settings.gpus.restartHint')}</Text> : null}
  </>;
}

// ---- Keyboard and mouse ----

function Input() {
  const st = perms.get('machines', 'shortcuts');
  return <>
    <Head title={tr('machines.set.nav.input')} text={tr('machines.set.input.text')} />
    <Section>
      <Row label={tr('machines.set.input.release')} value="❖Esc" note={tr('machines.set.input.releaseNote')} />
      <Row label={tr('machines.set.input.shortcuts')} value={tr(`machines.set.input.perm.${st === 'allow' ? 'allow' : st === 'deny' ? 'deny' : 'ask'}`)} note={tr('machines.set.input.shortcutsNote')}>
        <View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.set.input.privacy')} onClick={() => wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } })} /></View>
      </Row>
      <Row label={tr('machines.set.input.mouse')} last note={tr('machines.set.input.mouseNote')} />
    </Section>
  </>;
}
