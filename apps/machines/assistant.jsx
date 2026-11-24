// The New Machine assistant (the approved mock "Assistant"), in the library window: 1 Choose what to run, 2 a profile
// (CPU, memory, disk), 3 the name, 4 review, 5 create. An installer disc chosen or dropped on the window (any file it
// takes, dropped anywhere on the first step) adds the Detect step after Choose: lord-vm --iso-detect reads its volume
// label and its UEFI and El Torito boot entries in seccomp's strict mode (server/src/vm/vmiso.h), and the step names the
// system, the processors the disc starts on and whether it would be emulated here, before anything is made (detectPlan).
// The Name step says where the machine goes (the library, ~/Machines, and the space free there). An unattended install
// takes the snapshot "Fresh install" when it ends, before its first start (vmwindow.jsx; on by default, in the review).
// Download and install: the cards of assets/machines/systems.json for the chosen architecture (catalog.js); choosing
// one reads its maker's SHA-256 list (vm-image-list), the newest image of the release is named with its hash, and step 5
// downloads it (vm-image-get, which keeps it only with that hash) before the machine is made from it.
// Migrate from another app opens the import plan (migrate.jsx).
// Install without questions (the mock "Unattended", step 3 "Set up"): for a card whose catalog entry says how, the
// account, password, computer name, time zone and keyboard become the installer's answer file (lord-vm --unattended,
// vmunattend.h); the password goes to lord-server, which keeps only its hash. A card may install from another image
// of its maker's (Ubuntu's server image, Fedora's network installer), resolved like the card's own.
// Windows 11 (windows.js, 4.3): the person's own multi-edition ISO from Microsoft's page (found in Downloads and checked
// against Microsoft's published SHA-256) or Microsoft's 90-day evaluation (downloaded by the helper); the machine is
// made ready for it (secure boot, TPM 2.0, virtio-win drivers) and installs without questions or with Windows' own.
import { View, Text, TextInput, FilePicker, Toggle, ScrollView, admin, fs, openUrl, userDirs, settings, locale, useFilesDrop, FilesDropHint, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import CATALOG from '../../assets/machines/systems.json';
import { systemsFor, entryOf, listingNames, newest, pickImage, pinnedImage, cardSize, noticeOf, versionIn, progressOf, resumedOf, unattendedOf, checkSetup, hostnameFor, userFor, lordosLang } from './catalog.js';
import { validateDraft, createRequest, missingFor, systemOf, versionOf, archOf, suggestName, gfxFor, no3dReason, winfspFor, gvtGpu, defaultCard, detectPlan } from './model.js';
import { allProfiles, profileById, builtinProfile, defaultProfileId, onHost, setPatch, shareAbs, DEFAULT_PROFILE } from './profiles.js';
import { customUnattended, systemState } from './systems.js';
import { loadProfiles, loadSystems, defaultsOf, profileTitle } from './store.js';
import { usePal, OsMark, Btn, Bar, gb, bytes } from './ui.jsx';
import { SheetHeader, Pill, Round, Sym } from './kit.jsx';
import { GroupMark, groupColors } from './groupui.jsx';
import { writeMeta } from './meta.js';
import { Migrate } from './migrate.jsx';
import { windowsEntry, driversFor, matchIso, evalFiles, evalFor, newestDownload, windowsZone, inputLocale, userLocale, editionPlan, checkWindowsSetup, windowsHostname, windowsSize } from './windows.js';
import ZONES from '../../assets/machines/windows-zones.json';
import { GroupFlow, resolveGroup } from './whonix.jsx';

// what Migrate from another app takes: other apps' settings files and packages, and their disk formats
const MIGRATE_FILES = /(\.(vmx|vbox|pvs|ovf|ova|vmdk|vdi|vhdx|vhd|hds)|\/config\.pvs)$/i;
const DISK_FILES = /\.(qcow2|img|raw)$/i;
// a file dropped on the first step: which tile takes it
export const dropSource = (name) => (/\.iso$/i.test(name) ? 'iso' : DISK_FILES.test(name) ? 'import' : MIGRATE_FILES.test(`/${name}`) ? 'migrate' : null);

const STEPS = ['choose', 'profile', 'name', 'review', 'install'];
const rowsOf = (list, n) => { const out = []; for (let i = 0; i < list.length; i += n) out.push(list.slice(i, i + n)); return out; };

function Tile({ title, hint, onClick, selected, disabled, mark, height = 76, children }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flex: 1, height, padding: 12, gap: 6, justifyContent: mark ? 'space-between' : 'center', borderRadius: 10, opacity: disabled ? 0.5 : 1,
        border: `1px solid ${selected ? p.accent : p.line}`, backgroundColor: selected ? (p.dark ? '#1c3550' : '#e6f1ff') : hover && !disabled ? p.btn : p.card }}>
      {mark}
      <View>
        <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {hint ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{hint}</Text> : null}
      </View>
      {children}
    </View>
  );
}

export function Assistant({ host, mode, names, groupNames, onClose, onCreated, onConfigure, onNeeds, onGroup }) {
  const p = usePal();
  const [step, setStep] = useState(0);
  const [source, setSource] = useState(mode === 'import' ? 'import' : null);
  const [path, setPath] = useState('');
  const [picking, setPicking] = useState(mode === 'import');
  const [sys, setSys] = useState({ id: 'other', name: '' });
  const [ver, setVer] = useState('');
  const [arch, setArch] = useState(host.arch);
  // profiles (Machines settings → Profiles): LordOS's five and the person's own; the default one first chosen
  const [own] = useState(loadProfiles);
  const [customs] = useState(loadSystems);
  const [profile, setProfile] = useState(() => defaultProfileId(defaultsOf(settings.get().machinesDefaults).profile, own));
  const [custom, setCustom] = useState(null);     // a system the person added (systems.js), once its image is verified
  const [shareOn, setShareOn] = useState(true);   // the profile's shared folder, confirmed in the review
  const [gpuReady] = useState(() => { try { return !!gvtGpu(typeof lord !== 'undefined' && lord.vm?.gpus ? lord.vm.gpus() : null); } catch (e) { return false; } });
  const [name, setName] = useState('');
  const [rootPassword, setRootPassword] = useState('');
  const [customize, setCustomize] = useState(false);
  const [shareClip, setShareClip] = useState(false);
  // the Detect step (an installer disc): shown after Choose; detect = { path, phase: 'reading' | 'done' | 'error', info }
  const [detectPage, setDetectPage] = useState(false);
  const [detect, setDetect] = useState(null);
  const detectJob = useRef(0);
  const [freshSnap, setFreshSnap] = useState(true);   // the snapshot "Fresh install" when an unattended install ends
  const [job, setJob] = useState(0);
  const [fail, setFail] = useState(null);
  const [stopped, setStopped] = useState(false);  // Stop Download: the part is kept, Continue Download picks it up
  // a download card: { id, name, phase: 'list' | 'ready' | 'error', image: { name, sha, size }, err }
  const [dl, setDl] = useState(null);
  const [got, setGot] = useState(null);          // the download's progress, { done, total }, in step 5
  const pickSeq = useRef(0);
  // Install without questions: { on, user, password, hostname, timezone, keymap }; alt: the image it boots when that
  // isn't the card's own ({ id, phase, image, err })
  const hd = (typeof lord !== 'undefined' && lord.vm?.hostDefaults?.()) || { timezone: 'UTC', keymap: 'us', locale: 'en_US.UTF-8' };
  const [un, setUn] = useState({ on: true, user: userFor(typeof lord !== 'undefined' && lord.fs?.home ? lord.fs.home() : ''), password: '', hostname: '', timezone: hd.timezone, keymap: hd.keymap });
  const [alt, setAlt] = useState(null);
  const altSeq = useRef(0);
  // Windows: { mode: 'own' | 'eval', phase: 'wait' | 'hashing' | 'ready' | 'error', iso: { path, name, lang, label }, pct, err, lang }
  const [win, setWin] = useState(null);
  const [edition, setEdition] = useState('Windows 11 Pro');
  const [winKey, setWinKey] = useState('');
  const hashJob = useRef(0);
  const cancelled = useRef(false);
  const other = host.arch === 'amd64' ? 'arm64' : 'amd64';
  const windows = source === 'windows';
  const winE = windowsEntry(CATALOG, arch);
  const prof = profileById(profile, own, host) || builtinProfile(DEFAULT_PROFILE, host);
  // what the profile gives on this computer (half its processors at most, the memory machines may use here, a graphics
  // card only when one is set up for machines)
  const pv = onHost(prof, host, { gpuReady, no3d: arch !== host.arch ? 'arch' : no3dReason(host, windows, arch) });
  const base = { vcpus: pv.vcpus, memoryMb: pv.memoryMb, diskGb: pv.diskGb, network: pv.network, gfx: prof.resources.graphics };
  // Windows' minimums (4.3); a download card's own size: Android's machine whatever the profile (4.2), LordOS's least (4 processors, 4 GB, 32 GB, 3D)
  const size = windows ? windowsSize(base, host) : source === 'download' && dl ? cardSize(CATALOG, dl.id, base, host) : base;
  const home = typeof lord !== 'undefined' && lord.fs?.home ? lord.fs.home() : '';
  const shareDir = pv.share ? shareAbs(pv.share.path, home) : '';
  // a bridged machine joins the card chosen in Machines settings → Network (else the first wired one up): said in the review
  const bridgeCard = pv.network === 'bridged' ? defaultCard(host.cards, defaultsOf(settings.get().machinesDefaults).bridge) : '';
  const netText = [tr(`machines.cfg.net.${pv.network}`), bridgeCard || null, pv.ports.length ? tr('machines.cfg.sum.ports', { n: pv.ports.length }) : null].filter(Boolean).join(' · ');
  const shareThere = (() => { if (!shareDir) return false; try { return !!fs.stat(shareDir)?.dir || !!fs.stat(shareDir)?.isDir; } catch (e) { return false; } })();
  const downloading = source === 'download';
  const evalFile = windows && win?.mode === 'eval' ? evalFiles(winE).find((f) => f.lang === win.lang) || evalFor(winE, hd.locale) : null;
  const winPath = windows ? (win?.mode === 'eval' ? evalFile?.name || '' : win?.phase === 'ready' ? win.iso.path : '') : '';
  // a download is checked as the ISO it will be (its file name), and made from the downloaded path
  const d = { name, arch, vcpus: size.vcpus, memoryMb: size.memoryMb, diskGb: size.diskGb, network: size.network, source: source === 'iso' || source === 'custom' || downloading || windows ? 'iso' : 'import',
    path: downloading ? (dl?.image?.name || '') : windows ? winPath : path, rootPassword, shareClipboard: shareClip,
    // Design's 3D (6.10): Linux machines of this computer's architecture, where it can give them
    graphics: arch === host.arch ? gfxFor(size.gfx, host, windows, arch) : '2d' };
  const v = validateDraft(d, host, names);
  const missing = missingFor(host, [arch], { windows });
  const emulated = arch !== host.arch;

  const chose = (pth, src = source) => {
    setPath(pth);
    const file = pth.split('/').pop();
    const label = src === 'iso' ? (lord.vm.isoInfo(pth)?.label || '') : '';
    const s = systemOf(label, file);
    // a Windows ISO goes the Windows way: checked against Microsoft's list and made into the Windows 11 template
    if (src === 'iso' && s.id === 'windows' && windowsEntry(CATALOG, archOf(file) || archOf(label) || arch)) { pickWindows(); checkIso(pth); return; }
    if (src === 'iso') startDetect(pth); else setDetect(null);
    const vv = versionOf(label) || versionOf(file);
    setSys(s); setVer(vv);
    const a = archOf(file) || archOf(label);
    if (a) setArch(a);
    setName(suggestName(s, vv, names));
  };
  // the Detect step: lord-vm reads the disc (its boot files say what it starts on); the architecture follows them
  const startDetect = (pth) => {
    const id = lord.vm.isoDetect ? lord.vm.isoDetect(pth) : 0;
    detectJob.current = id;
    setDetect(id ? { path: pth, phase: 'reading', info: null } : { path: pth, phase: 'error', info: null });
  };
  useEffect(() => {
    if (detect?.phase !== 'reading') return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(detectJob.current);
      if (!r || !r.done) return;
      clearInterval(timer);
      let info = null;
      for (const l of String(r.out || '').trim().split('\n')) { try { const o = JSON.parse(l); if (o && typeof o === 'object' && 'iso' in o) info = o; } catch (e) { /* not ours */ } }
      setDetect((x) => (x && x.phase === 'reading' ? { ...x, phase: r.ok && info ? 'done' : 'error', info } : x));
      if (r.ok && info) {
        const dp = detectPlan(info, detect.path.split('/').pop(), host.arch);
        if (dp.from === 'boot') setArch(dp.arch);
      }
    }, 200);
    return () => clearInterval(timer);
  }, [detect?.phase, detect?.path]);
  // an entry's maker's list (and, when the list's name changes per build, the folder's listing first), checked by the
  // helper, and the newest image in it
  const resolve = async (id, a) => {
    const e = entryOf(CATALOG, id, a);
    if (!e) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
    // a pinned image (Android, 4.2): the signed catalog is its list
    const pin = pinnedImage(e);
    if (pin) return pin;
    let p = await admin.run('vm-image-list', `${id} ${a}`, null, { quiet: true });
    if (!e.sums) {
      const list = newest(listingNames(fs.read(String(p)), e.sumsPattern), e.sumsPattern);
      if (!list) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
      p = await admin.run('vm-image-list', `${id} ${a} ${list}`, null, { quiet: true });
    }
    const img = pickImage(e, fs.read(String(p)));
    if (!img) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
    return img;
  };
  // a card: its list, the newest image
  const pickWindows = () => {
    pickSeq.current++;
    setSource('windows'); setPath(''); setPicking(false); setDl(null); setAlt(null);
    setSys({ id: 'windows', name: 'Windows' }); setVer('11'); setName(suggestName({ name: 'Windows' }, '11', names));
    setWin(null);
  };
  // the person's ISO, hashed by lord-vm and looked up in Microsoft's published list
  const checkIso = (pth) => {
    const file = pth.split('/').pop();
    const id = lord.vm.hash(pth);
    if (!id) { setWin({ mode: 'own', phase: 'error', err: tr('machines.win.cantRead', { file }) }); return; }
    hashJob.current = id;
    setWin({ mode: 'own', phase: 'hashing', pct: 0, iso: { path: pth, name: file } });
  };
  useEffect(() => {
    if (!windows || win?.phase !== 'hashing') return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(hashJob.current);
      if (!r) return;
      const lines = String(r.out || '').trim().split('\n');
      let pct = 0, sha = '';
      for (const l of lines) { try { const o = JSON.parse(l); if (typeof o.progress === 'number') pct = o.progress; if (o.sha256) sha = o.sha256; } catch (e) { /* a partial line */ } }
      if (!r.done) { setWin((w) => (w?.phase === 'hashing' ? { ...w, pct } : w)); return; }
      const m = r.ok && sha ? matchIso(winE, sha) : null;
      setWin((w) => (!w || w.phase !== 'hashing' ? w : m ? { ...w, phase: 'ready', iso: { ...w.iso, ...m } }
        : { ...w, phase: 'error', err: tr(r.ok ? 'machines.win.notMicrosoft' : 'machines.win.cantRead', { file: w.iso.name }) }));
    }, 400);
    return () => clearInterval(timer);
  }, [windows, win?.phase]);
  // the ISO just downloaded from Microsoft's page, found in Downloads (a browser renames its .part when it's whole)
  useEffect(() => {
    if (!windows || win?.mode !== 'own' || win.phase !== 'wait') return undefined;
    const look = () => {
      try {
        const dir = userDirs().downloads;
        const f = newestDownload(lord.fs.list(dir).map((e) => ({ name: e.name, size: e.size, mtime: e.mtime, dir: e.dir || e.isDir })), arch);
        if (f) checkIso(`${dir}/${f.name}`);
      } catch (e) { /* no Downloads folder */ }
    };
    look();
    const timer = setInterval(look, 2000);
    return () => clearInterval(timer);
  }, [windows, win?.mode, win?.phase, arch]);
  useEffect(() => { if (windows) setUn((u) => ({ ...u, hostname: windowsHostname(name) })); }, [windows, name]);
  // the clipboard both ways when the profile says so (the review's switch starts there)
  useEffect(() => { setShareClip(pv.clipboard === 'both'); }, [profile]);
  // a system the person added: its verified image, the profile it says
  const pickCustom = (s) => {
    pickSeq.current++;
    setSource('custom'); setCustom(s); setDl(null); setAlt(null); setPicking(false); setPath(s.verified.path);
    setSys({ id: s.kind === 'windows' ? 'windows' : 'other', name: s.name }); setVer(''); setName(suggestName({ name: s.name }, '', names));
    if (profileById(s.profile, own, host)) setProfile(s.profile);
  };
  // a machine group's card (Whonix, docs/VIRTUALIZATION.md 19.8): its newest verified release's archive
  const pickGroup = async (s) => {
    const seq = ++pickSeq.current;
    setSource('group'); setPath(''); setPicking(false); setAlt(null);
    setSys({ id: s.id, name: s.name }); setVer(s.release);
    setDl({ id: s.id, name: s.name, phase: 'list' });
    try {
      const img = await resolveGroup(CATALOG, s.id, host.arch);
      if (seq !== pickSeq.current) return;
      setVer(img.release);
      setDl({ id: s.id, name: s.name, phase: 'ready', image: img });
    } catch (err) {
      if (seq === pickSeq.current) setDl({ id: s.id, name: s.name, phase: 'error', err: err.message || String(err) });
    }
  };
  const pickDownload = async (s, a = arch) => {
    if (s.kind === 'windows') { pickWindows(); return; }
    if (s.kind === 'group') { pickGroup(s); return; }
    const seq = ++pickSeq.current;
    setSource('download'); setPath(''); setPicking(false); setAlt(null);
    setSys({ id: s.id, name: s.name }); setVer(s.release); setName(suggestName({ name: s.name }, s.release, names));
    setDl({ id: s.id, name: s.name, phase: 'list' });
    try {
      const img = await resolve(s.id, a);
      if (seq !== pickSeq.current) return;
      const vv = versionIn(img.name, s.release);
      setVer(vv); setName(suggestName({ name: s.name }, vv, names));
      setDl({ id: s.id, name: s.name, phase: 'ready', image: img });
    } catch (err) {
      if (seq === pickSeq.current) setDl({ id: s.id, name: s.name, phase: 'error', err: err.message || String(err) });
    }
  };
  // another architecture: the same card for it
  useEffect(() => {
    if (source === 'group' && arch !== host.arch) { setSource(null); setDl(null); return; }
    if (!downloading || !dl) return;
    const s = systemsFor(CATALOG, arch).find((x) => x.id === dl.id);
    if (s) pickDownload(s, arch); else { setSource(null); setDl(null); }
  }, [arch]);
  // the image an unattended install boots, when it isn't the card's own: read once the switch is on
  const uo = downloading && dl ? unattendedOf(CATALOG, dl.id, arch) : source === 'custom' ? customUnattended(custom, CATALOG, arch) : null;
  const unOn = !!uo && un.on;
  const needAlt = unOn && downloading && uo.image !== dl?.id;
  useEffect(() => {
    if (!needAlt || (alt && alt.id === uo.image && alt.arch === arch && alt.phase !== 'error')) return;
    const seq = ++altSeq.current;
    setAlt({ id: uo.image, arch, phase: 'list' });
    resolve(uo.image, arch).then((img) => { if (seq === altSeq.current) setAlt({ id: uo.image, arch, phase: 'ready', image: img }); })
      .catch((err) => { if (seq === altSeq.current) setAlt({ id: uo.image, arch, phase: 'error', err: err.message || String(err) }); });
  }, [needAlt, uo?.image, arch]);
  useEffect(() => { if (name && !un.hostname && !windows) setUn((u) => ({ ...u, hostname: hostnameFor(name) })); }, [name]);
  const setup = windows ? checkWindowsSetup(un) : checkSetup(un);
  const plan = windows ? editionPlan(winE, win?.mode, edition, winKey) : null;
  // what is downloaded and installed: the card's image, or the unattended install's own
  const inst = needAlt ? (alt?.phase === 'ready' ? { id: alt.id, image: alt.image } : null) : downloading && dl?.image ? { id: dl.id, image: dl.image } : null;
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      if (r.ok) {
        // a Windows machine sets up its transfer folder (WinFsp) by itself once its guest tools are in (6.7.1)
        writeMeta(r.dir, { os: sys.id, osName: [sys.name, ver].filter(Boolean).join(' '), profile, onClose: pv.onClose, appsMode: pv.appsMode,
          // the mouse and shortcuts choices; capturing in full screen is machine.json's (setPatch below)
          input: { mouse: pv.input.mouse, shortcuts: pv.input.shortcuts },
          ...(source === 'custom' && custom?.kind === 'linux' ? { family: 'linux' } : {}), ...(windows ? { transferSetup: true } : {}),
          // the snapshot "Fresh install" when the unattended install ends (vmwindow.jsx, reason 'installed')
          ...((windows ? un.on : unOn) && freshSnap ? { freshSnap: true } : {}) });
        const m = lord.vm.list().find((x) => x.dir === r.dir);
        const done = () => { if (customize) onConfigure(m || null); else onCreated(m || null); };
        // what lord.vm.create doesn't take (the network mode and its ports, the clipboard's way, the transfer folder, the
        // shared folder, the graphics card): lord.vm.set while the new machine is still off
        const patch = setPatch(pv, { bridge: defaultCard(host.cards, defaultsOf(settings.get().machinesDefaults).bridge), shareAbsPath: shareOn && shareThere ? shareDir : '' });
        const sid = Object.keys(patch).length ? lord.vm.set(r.dir, patch) : 0;
        if (!sid) { done(); return; }
        const t2 = setInterval(() => {
          const q = lord.vm.job(sid);
          if (q && !q.done) return;
          clearInterval(t2);
          if (q && !q.ok) setFail(q.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || tr('machines.cfg.refused'));
          else done();
        }, 300);
      } else setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim() || '?');
    }, 300);
    return () => clearInterval(timer);
  }, [job]);
  // a file the helper downloads and keeps only with its hash, with the progress on the page
  const fetchImage = (id, f) => {
    setGot({ done: 0, total: f.size || 0, file: f.name });
    return admin.run('vm-image-get', `${id} ${arch} ${f.name} ${f.sha}`, (l) => {
      const g = progressOf(l);
      if (g) setGot((o) => ({ ...g, file: f.name, resumed: o?.resumed || 0 }));
      const r = resumedOf(l);
      if (r) setGot((o) => ({ total: 0, ...o, done: Math.max(o?.done || 0, r), resumed: r }));
    }, { quiet: true }).then(String);
  };
  const createWindows = async () => {
    setStep(4); cancelled.current = false;
    let iso, drivers;
    try {
      iso = win.mode === 'eval' ? await fetchImage('windows', evalFile) : win.iso.path;
      drivers = await fetchImage('virtio-win', driversFor(CATALOG, arch));
      // WinFsp for the transfer folder, kept in the image store for the machine's first sign-in; not needed to install
      const fsp = winfspFor(CATALOG, arch);
      if (fsp) { try { await fetchImage('winfsp', fsp); } catch (e) { if (cancelled.current) throw e; } }
    } catch (e) {
      setGot(null);
      if (cancelled.current) { setStopped(true); return; }
      setFail(e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed'));
      return;
    }
    setGot(null);
    const lang = win.mode === 'eval' ? evalFile.lang : win.iso.lang;
    const req = createRequest({ ...d, path: iso });
    req.windows = { drivers };
    req.unattended = { kind: 'windows', guided: !un.on, user: un.user, password: un.password, fullname: '', hostname: un.hostname,
      edition: plan.name, key: plan.key, uilang: lang, inputloc: inputLocale(un.keymap, '', lang), userloc: userLocale(hd.locale), wtz: windowsZone(ZONES, un.timezone) };
    const id = lord.vm.create(req);
    req.unattended.password = ''; setUn((u) => ({ ...u, password: '' }));
    if (!id) { setFail(tr('machines.new.refused')); return; }
    setJob(id);
  };
  const create = async () => {
    setFail(null); setStopped(false);
    if (windows) { await createWindows(); return; }
    let iso = null;
    if (source === 'custom') {
      // a system the person added: its image hashed again now, and used only if it is still the one they verified
      setStep(4); setGot({ done: 0, total: 100, checking: true });
      const ok = await new Promise((res) => {
        const id = lord.vm.hash(custom.verified.path);
        if (!id) { res(false); return; }
        const t3 = setInterval(() => {
          const r = lord.vm.job(id);
          if (!r) return;
          let pct = 0, sha = '';
          for (const l of String(r.out || '').trim().split('\n')) { try { const o = JSON.parse(l); if (typeof o.progress === 'number') pct = o.progress; if (o.sha256) sha = o.sha256; } catch (e) { /* partial */ } }
          if (!r.done) { setGot({ done: pct, total: 100, checking: true }); return; }
          clearInterval(t3);
          res(r.ok && sha.toLowerCase() === custom.sha256);
        }, 400);
      });
      setGot(null);
      if (!ok) { setFail(tr('machines.set.sys.shaMismatch', { file: custom.verified.path.split('/').pop() })); return; }
      iso = custom.verified.path;
    }
    if (downloading) {
      setStep(4); setGot({ done: 0, total: inst.image.size || 0 }); cancelled.current = false;
      try {
        // a download stopped before is continued: the helper says from where first ("vm.downloadResumed")
        iso = String(await admin.run('vm-image-get', `${inst.id} ${arch} ${inst.image.name} ${inst.image.sha}`, (l) => {
          const g = progressOf(l);
          if (g) setGot((o) => ({ ...g, resumed: o?.resumed || 0 }));
          const r = resumedOf(l);
          if (r) setGot((o) => ({ total: 0, ...o, done: Math.max(o?.done || 0, r), resumed: r }));
        }, { quiet: true }));
      } catch (e) {
        setGot(null);
        if (cancelled.current) { setStopped(true); return; }
        setFail(e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed'));
        return;
      }
      setGot(null);
    }
    const req = createRequest(iso ? { ...d, path: iso } : d);
    if (iso && unOn) {
      req.unattended = { kind: uo.kind, user: un.user, password: un.password, fullname: '', hostname: un.hostname, timezone: un.timezone,
        keymap: un.keymap, variant: '', locale: hd.locale || 'en_US.UTF-8', release: uo.release, kernel: uo.kernel, initrd: uo.initrd, append: uo.append, desktop: true };
      // a LordOS machine starts in this LordOS's language
      if (uo.kind === 'lordos') req.unattended.lang = lordosLang(locale.get().lang);
    }
    const id = lord.vm.create(req);
    if (req.unattended) { req.unattended.password = ''; setUn((u) => ({ ...u, password: '' })); }
    if (!id) { setFail(tr('machines.new.refused')); setStep(4); return; }
    setJob(id);
    setStep(4);
  };
  const stopDownload = () => { cancelled.current = true; admin.run('vm-image-cancel', '', null, { quiet: true }).catch(() => {}); };

  const grp = source === 'group';
  if (grp && step > 0 && dl?.phase === 'ready') return <GroupFlow step={step} setStep={setStep} dl={dl} host={host} names={names} groupNames={groupNames} catalog={CATALOG}
    onCancel={onClose} onGroup={(dir) => onGroup?.(dir)} />;
  if (source === 'migrate' && path && !picking) return <Migrate host={host} path={path} names={names} onClose={onClose} onDone={() => onClose()} onStart={(m) => onCreated(m)} />;
  const can = grp ? [dl?.phase === 'ready', false, false, false, false][step] : windows ? [win?.mode === 'eval' ? !!evalFile : win?.phase === 'ready', true, !v.errors.name && (!un.on || (setup.ok && plan.ok)),
    v.ok && !missing.length, false][step]
    : [downloading ? dl?.phase === 'ready' : !!source && !!path && !v.errors.path, true, !v.errors.name && (!unOn || (setup.ok && !!inst)),
      v.ok && !missing.length && (!downloading || !!inst), false][step];
  const title = step === 2 && (uo || windows) ? tr('machines.as.title.setup') : tr(`machines.as.title.${STEPS[step]}`);
  const fileHint = (k) => (source === k && path ? path.split('/').pop() : tr(`machines.as.${k}Hint`));
  let body = null;
  if (step === 0) body = <>
    <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.as.download')}</Text>
    {/* the mock's grid of four: one row per four systems of the catalog, for the chosen architecture */}
    {rowsOf(systemsFor(CATALOG, arch, { kvm: host.kvm, hostArch: host.arch }), 4).map((row, i) => (
      <View key={i} style={{ flexDirection: 'row', gap: 12 }}>
        {row.map((s) => {
          const mine = ((downloading || source === 'group') && dl?.id === s.id) || (windows && s.kind === 'windows');
          const exact = mine && dl?.image?.size > 0;
          const title = [s.name, mine && dl?.phase === 'ready' ? ver : s.release].filter(Boolean).join(' ');
          const sz = bytes(exact ? dl.image.size : s.approxMb * 1048576);
          // a group's card: "Whonix (Tor)", the group's mark and its "Group · 2 machines" badge
          if (s.kind === 'group') return <Tile key={s.id} height={118} selected={mine} mark={<GroupTileMark n={(s.members || []).length || 2} />}
            title={tr('machines.whonix.card', { name: s.name })} hint={tr('machines.whonix.cardHint', { size: sz, mem: gb((s.members || []).reduce((a, m) => a + m.memoryMb, 0)) })} onClick={() => pickDownload(s)} />;
          return <Tile key={s.id} height={118} selected={mine} mark={<OsMark id={s.id} size={40} />} title={title}
            hint={tr(exact ? 'machines.dl.size' : 'machines.dl.sizeAbout', { arch, size: sz })} onClick={() => pickDownload(s)} />;
        })}
        {row.length < 4 ? Array.from({ length: 4 - row.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />) : null}
      </View>
    ))}
    {customs.length ? <>
      <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.set.sys.yours')}</Text>
      {rowsOf(customs, 4).map((row, i) => (
        <View key={`c${i}`} style={{ flexDirection: 'row', gap: 12 }}>
          {row.map((s) => {
            const ready = systemState(s) === 'ready';
            return <Tile key={s.id} height={118} selected={source === 'custom' && custom?.id === s.id} disabled={!ready} mark={<OsMark id="other" color={s.color} letter={s.name} size={40} />}
              title={s.name} hint={ready ? tr('machines.set.sys.verified') : tr('machines.as.customNotReady')} onClick={() => pickCustom(s)} />;
          })}
          {row.length < 4 ? Array.from({ length: 4 - row.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />) : null}
        </View>
      ))}
    </> : null}
    <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.as.own')}</Text>
    <View style={{ flexDirection: 'row', gap: 12 }}>
      <Tile title={tr('machines.as.iso')} hint={fileHint('iso')} selected={source === 'iso'} onClick={() => { pickSeq.current++; setDl(null); if (source !== 'iso') setPath(''); setSource('iso'); setPicking(true); }} />
      <Tile title={tr('machines.as.migrate')} hint={tr('machines.as.migrateHint')} selected={source === 'migrate'} onClick={() => { pickSeq.current++; setDl(null); setPath(''); setSource('migrate'); setPicking(true); }} />
      <Tile title={tr('machines.as.import')} hint={fileHint('import')} selected={source === 'import'} onClick={() => { pickSeq.current++; setDl(null); if (source !== 'import') setPath(''); setSource('import'); setPicking(true); }} />
    </View>
    {windows ? <WinChoose win={win} setWin={setWin} entry={winE} arch={arch} lang={hd.locale} onPick={() => setPicking(true)} /> : null}
    {(downloading || grp) && dl ? (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 12, borderRadius: 10, backgroundColor: p.raised }}>
        <OsMark id={dl.id} size={36} />
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{`${[dl.name, ver].filter(Boolean).join(' ')}, ${arch}`}</Text>
          <Text style={{ fontSize: 12, color: dl.phase === 'error' ? '#ff453a' : p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>
            {dl.phase === 'list' ? tr('machines.dl.reading', { name: dl.name }) : dl.phase === 'error' ? dl.err : grp ? tr('machines.whonix.found', { file: dl.image.name, release: dl.image.release })
              : tr(dl.image.pinned ? 'machines.dl.pinned' : 'machines.dl.found', { file: dl.image.name })}</Text>
        </View>
        {dl.phase === 'error' ? <Btn label={tr('machines.dl.retry')} onClick={() => { const s = systemsFor(CATALOG, arch).find((x) => x.id === dl.id); if (s) pickDownload(s); }} /> : null}
      </View>
    ) : null}
    {downloading && dl && noticeOf(CATALOG, dl.id) ? (() => { const nt = noticeOf(CATALOG, dl.id); return (
      <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr(`machines.dl.notice.${nt.key}`, { maker: nt.maker, date: nt.date })}</Text>
      </View>); })() : null}
    {path && !downloading && !windows ? (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 12, borderRadius: 10, backgroundColor: p.raised }}>
        <OsMark id={sys.id} size={36} />
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{[sys.name || tr('machines.as.unknownSystem'), ver].filter(Boolean).join(' ')}{`, ${arch}`}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{path}</Text>
        </View>
      </View>
    ) : null}
    {emulated ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.as.emulated', { arch, host: host.arch })}</Text></View> : null}
  </>;
  else if (step === 1) {
    // the mock "Profiles": two columns of cards, each with what it's for and what it gives on this computer
    const all = allProfiles(own, host);
    const rows = [];
    for (let i = 0; i < all.length; i += 2) rows.push(all.slice(i, i + 2));
    body = <>
    <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.as.profileFits', { cpus: host.cpus, mem: gb(host.ramMb) })}</Text>
    {rows.map((r, i) => (
      <View key={i} style={{ flexDirection: 'row', gap: 12 }}>
        {r.map((pr) => { const k = pr.id; const s = onHost(pr, host, { gpuReady, no3d: arch !== host.arch ? 'arch' : no3dReason(host, windows, arch) }); return (
          <View key={k} onClick={() => setProfile(k)} style={{ flex: 1, minHeight: 92, paddingHorizontal: 14, paddingVertical: 10, gap: 4, borderRadius: 10,
            backgroundColor: p.card, border: `2px solid ${profile === k ? p.accent : p.line}` }}>
            <Text style={{ fontSize: 14, fontWeight: 600, color: p.text, textAlign: 'start' }}>{profileTitle(pr)}</Text>
            <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{pr.builtin ? tr(`machines.as.profileHint.${k}`) : pr.description || tr('machines.set.prof.yours')}</Text>
            <Text style={{ fontSize: 12, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.as.spec', { cpus: s.vcpus, mem: gb(s.memoryMb), gfx: tr(`machines.as.gfx.${pr.resources.graphics}`) })}</Text>
          </View>); })}
        {r.length < 2 ? <View style={{ flex: 1 }} /> : null}
      </View>
    ))}
    {size.gfx === '3d' && d.graphics !== '3d' ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>
      {[tr('machines.as.no3d'), tr(`machines.cfg.no3d.${arch !== host.arch ? 'arch' : no3dReason(host, windows, arch) || 'gpu'}`)].join(' ')}</Text> : null}
    {emulated ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.as.emulatedShort')}</Text></View> : null}
  </>;
  }
  else if (step === 2) body = <>
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14 }}>
      <OsMark id={sys.id} size={56} />
      <View style={{ flex: 1, gap: 4 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.new.name')}</Text>
        <TextInput value={name} onChange={setName} autoFocus placeholder={tr('machines.new.namePlaceholder')} />
        {v.errors.name && name ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.new.err.${v.errors.name}`)}</Text> : null}
      </View>
    </View>
    {source === 'import' ? (
      <View style={{ gap: 4 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.new.rootPassword')}</Text>
        <TextInput secure value={rootPassword} onChange={setRootPassword} />
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{tr('machines.new.rootPasswordHint')}</Text>
      </View>
    ) : null}
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <Toggle on={customize} onChange={setCustomize} />
      <Text style={{ fontSize: 13, color: p.text }}>{tr('machines.as.customize')}</Text>
    </View>
    {windows ? <WinSetup un={un} setUn={setUn} setup={setup} plan={plan} entry={winE} mode={win?.mode} edition={edition} setEdition={setEdition}
      winKey={winKey} setWinKey={setWinKey} lang={win?.mode === 'eval' ? evalFile?.lang : win?.iso?.lang} /> : null}
    {uo ? <>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 14, borderRadius: 12, backgroundColor: p.dark ? '#1d3049' : '#e6f1ff', border: `1px solid ${p.dark ? '#2f5580' : '#9cc6f5'}` }}>
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.un.title')}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.un.hint')}</Text>
        </View>
        <Toggle on={un.on} onChange={(on) => setUn((u) => ({ ...u, on }))} />
      </View>
      {un.on ? <>
        <View style={{ flexDirection: 'row', gap: 12 }}>
          <View style={{ flex: 1, gap: 4 }}>
            <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.un.user')}</Text>
            <TextInput value={un.user} onChange={(user) => setUn((u) => ({ ...u, user }))} />
            {setup.errors.user ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.un.err.${setup.errors.user}`)}</Text> : null}
          </View>
          <View style={{ flex: 1, gap: 4 }}>
            <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.un.password')}</Text>
            <TextInput secure value={un.password} onChange={(password) => setUn((u) => ({ ...u, password }))} />
            {setup.errors.password && un.password ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.un.err.${setup.errors.password}`)}</Text> : null}
          </View>
        </View>
        <View style={{ flexDirection: 'row', gap: 12 }}>
          {[['hostname', 'computer'], ['timezone', 'timezone'], ['keymap', 'keyboard']].map(([k, l]) => (
            <View key={k} style={{ flex: 1, gap: 4 }}>
              <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr(`machines.un.${l}`)}</Text>
              <TextInput value={un[k]} onChange={(val) => setUn((u) => ({ ...u, [k]: val }))} />
              {k === 'hostname' && setup.errors.hostname ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr('machines.un.err.hostname')}</Text> : null}
            </View>
          ))}
        </View>
        <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr(`machines.un.how.${uo.kind}`)}</Text>
        {needAlt ? <Text style={{ fontSize: 12, color: alt?.phase === 'error' ? '#ff453a' : p.dim, textAlign: 'start' }}>
          {alt?.phase === 'ready' ? tr('machines.un.image', { file: alt.image.name }) : alt?.phase === 'error' ? alt.err : tr('machines.dl.reading', { name: dl.name })}</Text> : null}
      </> : null}
    </> : null}
  </>;
  else if (step === 3) {
    const winRows = !windows ? [] : [
      [tr('machines.win.r.edition'), win?.mode === 'eval' ? tr('machines.win.evalName') : un.on ? plan?.name || '' : tr('machines.win.r.chosenInSetup')],
      [tr('machines.as.r.download'), win?.mode === 'eval' ? tr('machines.win.r.evalDownload', { size: bytes(evalFile?.size || 0) }) : tr('machines.win.r.ownIso')],
      [tr('machines.win.r.drivers'), tr('machines.win.r.driversValue', { size: bytes(driversFor(CATALOG, arch)?.size || 0) })],
      [tr('machines.win.r.ready'), tr('machines.win.template')],
      [tr('machines.as.r.install'), un.on ? tr('machines.un.review', { user: un.user, host: un.hostname }) : tr('machines.win.r.guided')],
    ];
    const rows = windows ? [
      [tr('machines.as.r.system'), [sys.name, ver].filter(Boolean).join(' ')],
      [tr('machines.as.r.from'), win?.mode === 'eval' ? evalFile?.name || '' : win?.iso?.name || ''],
      ...winRows,
      [tr('machines.as.r.arch'), arch],
      [tr('machines.as.r.cpu'), tr('machines.vcpus', { n: size.vcpus })],
      [tr('machines.as.r.memory'), gb(size.memoryMb)],
      [tr('machines.as.r.disk'), tr('machines.as.r.diskNew', { n: size.diskGb })],
      [tr('machines.as.r.network'), netText],
      [tr('machines.as.r.where'), `~/Machines/${name}.lordvm`],
    ] : [
      [tr('machines.as.r.system'), [sys.name || tr('machines.as.unknownSystem'), ver].filter(Boolean).join(' ')],
      [tr('machines.as.r.from'), downloading ? inst?.image.name || '' : path.split('/').pop()],
      ...(downloading ? [[tr('machines.as.r.download'), inst?.image.size ? tr('machines.dl.checked', { size: bytes(inst.image.size) }) : tr('machines.dl.checkedNoSize')]] : []),
      ...(uo ? [[tr('machines.as.r.install'), unOn ? tr('machines.un.review', { user: un.user, host: un.hostname }) : tr('machines.un.reviewOff')]] : []),
      [tr('machines.as.r.arch'), emulated ? `${arch}, ${tr('machines.emulatedLower')}` : arch],
      [tr('machines.as.r.cpu'), tr('machines.vcpus', { n: size.vcpus })],
      [tr('machines.as.r.memory'), gb(size.memoryMb)],
      [tr('machines.as.r.disk'), source === 'iso' || source === 'custom' || downloading ? tr('machines.as.r.diskNew', { n: size.diskGb }) : tr('machines.as.r.diskCopy', { n: size.diskGb })],
      [tr('machines.as.r.profile'), profileTitle(prof)],
      [tr('machines.as.r.network'), netText],
      [tr('machines.as.r.where'), `~/Machines/${name}.lordvm`],
    ];
    body = <>
      <View style={{ borderRadius: 12, backgroundColor: p.card }}>
        {rows.map(([k, val], i) => (
          <View key={k} style={{ flexDirection: 'row', paddingHorizontal: 14, paddingVertical: rows.length > 10 ? 7 : 10, borderBottomWidth: i < rows.length - 1 ? 1 : 0, borderColor: p.line }}>
            <Text style={{ flex: 1, fontSize: 13, color: p.dim, textAlign: 'start' }}>{k}</Text>
            <Text style={{ fontSize: 13, fontWeight: 600, color: p.text, direction: 'ltr' }}>{val}</Text>
          </View>
        ))}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12, borderRadius: 10, backgroundColor: p.card }}>
        <Toggle on={shareClip} onChange={setShareClip} />
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.as.shareClip')}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.as.shareClipHint')}</Text>
        </View>
      </View>
      {pv.share ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12, borderRadius: 10, backgroundColor: p.card }}>
          <Toggle on={shareOn && shareThere} onChange={setShareOn} />
          <View style={{ flex: 1 }}>
            <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.as.shareFolder', { path: pv.share.path, how: tr(pv.share.writable ? 'machines.set.imp.write' : 'machines.set.imp.read') })}</Text>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{shareThere ? tr('machines.as.shareFolderHint') : tr('machines.as.shareFolderMissing')}</Text>
          </View>
        </View>
      ) : null}
      {missing.length ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}>
          <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.new.needs', { list: missing.join(', ') })}</Text>
          <Btn label={tr('machines.req.button')} onClick={onNeeds} />
        </View>
      ) : null}
      {Object.keys(v.errors).length ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{Object.values(v.errors).map((e) => tr(`machines.new.err.${e}`, { max: host.memMaxMb })).join(' ')}</Text> : null}
    </>;
  } else if (stopped) {
    body = (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
        <OsMark id={sys.id} size={72} />
        <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{tr('machines.dl.stoppedTitle')}</Text>
        <Text style={{ maxWidth: 560, fontSize: 13, color: p.dim, textAlign: 'center' }}>{tr('machines.dl.cancelled')}</Text>
      </View>
    );
  } else if (got && !fail) {
    // the download, with its progress; the helper checks the SHA-256 at the end
    const mb = bytes;
    const checking = got.checking || (got.total > 0 && got.done >= got.total);
    body = (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
        <OsMark id={sys.id} size={72} />
        <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{got.checking ? tr('machines.as.checkingImage', { name: sys.name }) : tr('machines.dl.downloading', { name: windows && got.file ? got.file : [sys.name, ver].filter(Boolean).join(' ') })}</Text>
        <View style={{ width: 360 }}><Bar frac={got.total ? got.done / got.total : 0} color={p.accent} /></View>
        <Text style={{ fontSize: 13, color: p.dim }}>{checking ? tr('machines.dl.checking') : got.total ? tr('machines.dl.progress', { a: mb(got.done), b: mb(got.total) }) : mb(got.done)}</Text>
        {got.resumed > 0 && !checking ? <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.dl.resumed', { size: mb(got.resumed) })}</Text> : null}
        <Btn label={tr('machines.dl.cancel')} onClick={stopDownload} />
      </View>
    );
  } else body = (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
      <OsMark id={sys.id} size={72} />
      <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{fail ? (downloading && !job ? tr('machines.dl.failed') : tr('machines.as.failed')) : source === 'iso' || source === 'custom' || downloading ? tr('machines.new.creating') : tr('machines.new.importing')}</Text>
      {fail ? <Text style={{ maxWidth: 560, fontSize: 13, color: '#ff453a', textAlign: 'center' }}>{fail}</Text>
        : <Text style={{ fontSize: 13, color: p.dim }}>{tr('machines.as.thenStart')}</Text>}
    </View>
  );

  // a sheet over the library (kit.jsx): its title bar with Back, the step's content, the buttons at the bottom
  const back = step > 0 && !(step === 4 && !fail && !stopped) ? () => (stopped ? (setStopped(false), setStep(3)) : setStep(step - 1)) : null;
  return (
    <View style={{ flex: 1 }}>
      <SheetHeader onBack={back} title={tr('machines.newMachine')}
        subtitle={`${tr('machines.as.stepOf', { n: step + 1, total: STEPS.length })} · ${tr(`machines.as.step.${STEPS[step] === 'name' && (uo || windows) ? 'setup' : STEPS[step]}`)}`}
        right={<Round title={tr('machines.cancel')} onClick={onClose}><Text style={{ fontSize: 15, color: p.dim }}>✕</Text></Round>} />
      <View style={{ flex: 1, minHeight: 0, paddingHorizontal: 28, paddingVertical: 20, gap: 16 }}>
        <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 14, paddingBottom: 8 }}>{body}</ScrollView>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingVertical: 14, borderTopWidth: 1, borderColor: p.line }}>
        {step === 0 ? <Btn link label={arch === host.arch ? tr('machines.as.otherArch', { arch: other }) : tr('machines.as.thisArch', { arch: host.arch })}
          onClick={() => setArch(arch === host.arch ? other : host.arch)} /> : null}
        <View style={{ flex: 1 }} />
        {step === 4 && !fail && !stopped ? null : <Pill label={step === 0 || fail || stopped ? tr('machines.cancel') : tr('machines.back')} onClick={() => (step === 0 || fail || stopped ? onClose() : setStep(step - 1))} />}
        {stopped ? <Pill primary label={tr('machines.dl.continue')} onClick={create} /> : null}
        {step < 3 ? <Pill primary label={tr('machines.as.continue')} disabled={!can} onClick={() => setStep(step + 1)} /> : null}
        {step === 3 ? <Pill primary label={customize ? tr('machines.as.createConfigure') : tr('machines.as.createStart')} disabled={!can} onClick={create} /> : null}
      </View>
      {picking ? <FilePicker title={source === 'iso' || windows ? tr('machines.new.pickIso') : source === 'migrate' ? tr('machines.mig.pick') : tr('machines.new.pickImage')}
        filter={source === 'iso' || windows ? /\.iso$/i : source === 'migrate' ? MIGRATE_FILES : /\.(qcow2|img|raw)$/i}
        onPick={(x) => { const pth = Array.isArray(x) ? x[0] : x; if (windows) checkIso(pth); else if (source === 'migrate') setPath(pth); else chose(pth); setPicking(false); }}
        onClose={() => { setPicking(false); if (source === 'migrate' && !path) setSource(null); }} /> : null}
    </View>
  );
}

// a group card's mark: the group's two machines and a badge "Group · 2 machines"
function GroupTileMark({ n }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
      <View style={{ width: 56, height: 40, borderRadius: 8, alignItems: 'center', justifyContent: 'center', background: `linear-gradient(135deg, ${groupColors[0]}, ${groupColors[1]})` }}><GroupMark size={26} /></View>
      <View style={{ height: 20, paddingHorizontal: 8, borderRadius: 10, justifyContent: 'center', backgroundColor: 'rgba(125,70,152,0.85)' }}>
        <Text style={{ fontSize: 11, fontWeight: 700, color: '#fff', whiteSpace: 'nowrap' }}>{tr('machines.whonix.badge', { n })}</Text>
      </View>
    </View>
  );
}

// Windows, step 1: the person's own ISO (from Microsoft's page, found in Downloads) or the 90-day evaluation
function WinChoose({ win, setWin, entry, arch, lang, onPick }) {
  const p = usePal();
  const files = evalFiles(entry);
  const box = { padding: 14, gap: 8, borderRadius: 12, backgroundColor: p.card, border: `1px solid ${p.line}` };
  const evalLang = win?.mode === 'eval' ? win.lang : evalFor(entry, lang)?.lang;
  return <>
    <View style={{ flexDirection: 'row', gap: 12 }}>
      <Tile title={tr('machines.win.own')} hint={tr('machines.win.ownHint')} selected={win?.mode === 'own'} onClick={() => setWin({ mode: 'own', phase: 'wait' })} />
      <Tile title={tr('machines.win.eval')} hint={tr('machines.win.evalHint')} selected={win?.mode === 'eval'} disabled={!files.length}
        onClick={() => setWin({ mode: 'eval', phase: 'ready', lang: evalLang })} />
    </View>
    {win?.mode === 'own' ? (
      <View style={box}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.win.ownSteps', { arch: arch === 'arm64' ? 'Arm64' : 'x64' })}</Text>
        <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
          <Btn primary label={tr('machines.win.openPage')} onClick={() => openUrl(entry.page)} />
          <Btn label={tr('machines.win.choose')} onClick={onPick} />
        </View>
        {win.phase === 'wait' ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.win.waiting')}</Text> : null}
        {win.phase === 'hashing' ? <View style={{ gap: 6 }}>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.win.hashing', { file: win.iso.name, pct: win.pct || 0 })}</Text>
          <Bar frac={(win.pct || 0) / 100} color={p.accent} />
        </View> : null}
        {win.phase === 'ready' ? <Text style={{ fontSize: 12, fontWeight: 600, color: '#30d158', textAlign: 'start' }}>{tr('machines.win.verified', { file: win.iso.name, lang: win.iso.label })}</Text> : null}
        {win.phase === 'error' ? <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
          <Text style={{ flex: 1, fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{win.err}</Text>
          <Btn label={tr('machines.dl.retry')} onClick={() => setWin({ mode: 'own', phase: 'wait' })} />
        </View> : null}
      </View>
    ) : null}
    {win?.mode === 'eval' ? (
      <View style={box}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.win.evalNote', { days: entry?.evaluation?.days || 90 })}</Text>
        <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim }}>{tr('machines.win.lang')}</Text>
          {files.map((f) => <Btn key={f.lang} label={f.lang} primary={f.lang === evalLang} onClick={() => setWin({ mode: 'eval', phase: 'ready', lang: f.lang })} />)}
        </View>
      </View>
    ) : null}
    <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.win.legal')}</Text>
  </>;
}

// Windows, step 3: without questions (account, computer, edition and key) or Windows' own installer
function WinSetup({ un, setUn, setup, plan, entry, mode, edition, setEdition, winKey, setWinKey, lang }) {
  const p = usePal();
  const field = (k, l, secure) => (
    <View style={{ flex: 1, gap: 4 }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr(l)}</Text>
      <TextInput secure={secure} value={un[k]} onChange={(val) => setUn((u) => ({ ...u, [k]: val }))} />
      {setup.errors[k] && (k !== 'password' || un.password) ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.un.err.${setup.errors[k]}`)}</Text> : null}
    </View>
  );
  return <>
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 14, borderRadius: 12, backgroundColor: p.dark ? '#1d3049' : '#e6f1ff', border: `1px solid ${p.dark ? '#2f5580' : '#9cc6f5'}` }}>
      <View style={{ flex: 1 }}>
        <Text style={{ fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.un.title')}</Text>
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(un.on ? 'machines.win.how' : 'machines.win.howGuided')}</Text>
      </View>
      <Toggle on={un.on} onChange={(on) => setUn((u) => ({ ...u, on }))} />
    </View>
    {un.on ? <>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        {field('user', 'machines.un.user')}
        {field('password', 'machines.win.password', true)}
        {field('hostname', 'machines.un.computer')}
      </View>
      {mode === 'own' ? <>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignItems: 'center' }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim }}>{tr('machines.win.r.edition')}</Text>
          {(entry?.editions || []).map((e) => <Btn key={e.name} label={e.name.replace(/^Windows 11 /, '')} primary={e.name === edition} onClick={() => setEdition(e.name)} />)}
        </View>
        <View style={{ gap: 4 }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.win.key')}</Text>
          <TextInput value={winKey} onChange={setWinKey} placeholder="XXXXX-XXXXX-XXXXX-XXXXX-XXXXX" />
          <Text style={{ fontSize: 12, lineHeight: 1.4, color: plan?.err ? '#ff453a' : p.dim, textAlign: 'start' }}>
            {plan?.err === 'needsKey' ? tr('machines.win.needsKey') : plan?.err === 'key' ? tr('machines.win.badKey') : plan?.generic ? tr('machines.win.keyHint', { edition: plan.name }) : tr('machines.win.keyYours')}</Text>
        </View>
      </> : null}
      <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.win.regional', { lang: lang || '', keyboard: un.keymap, tz: un.timezone })}</Text>
    </> : null}
  </>;
}
