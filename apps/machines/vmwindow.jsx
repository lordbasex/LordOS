// A machine's window (the approved mocks "Toolbar", "FullScreen" and "BootFailed"): the guest's picture scaled to fit,
// keyboard and pointer while the window is in front, and a toolbar with the machine's name and power light, the
// resolution, Devices, Logs, Screenshot, Pause, Power, Configure and Full Screen; the same actions in the menu bar
// (Actions, Devices). In full screen the bar floats at the top edge. When the machine fails to start or stops with an
// error, the window shows why, a likely fix, the log and Start Again.
import {
  View, Text, Image, MediaView, Segmented, ScrollView, fs, FilePicker, Toggle, Terminal, Overlay, term, addListener, media, screen, openFileWithApps, useAppWindow, useWindowState, useStore, wm, windows, clipboard, notify, revealPath, openFolder,
  inhibitShortcuts, useNarrow, requestPermission, markDeviceUse, useFilesDrop, FilesDropHint, admin, t as tr, useSound, microphones,
  hasTextFocus, settings, useWindowsOutOfSight, displayFor, workAreaOf, direction,
} from '@lordos/sdk';
import CATALOG from '../../assets/machines/systems.json';
import { useEffect, useRef, useState } from 'react';
import { installError, startPhase, playStarts, stateView, offers2d, restartsAgain, offHint, actionsFor, toPicture, guestSize, refreshRate, wheelSteps, budgetPlan, failCause, failActions, pipFrame, actualFrame, infoLines, isolationGaps, usage, CTRL_ALT_DEL, windowTitle, closeChoice, resolutions, agentLines, agentTone, clipNeedsTool, clipTo, clipFrom, clipInUse, clipBlocked, unreadableOf, clipAskEmpty, clipAskRequest, clipAskPush, clipAskCopied, clipAskAnswer, clipAskTick, clipAskShown, thumbSrc, AGENT_CAPS, guestWindows, mountCommand, noSuspend, soundOf, soundDevs, audible, muteToggle, micLive, micAsk, dropBlocker, copyLine, midName, guestFolder, followStuck, appsBlocker, transferState, dropWay, winfspFor, autoTransferSetup, toolsOutdated, runningWindows, runningKey, launcherApps, latchToggle, latchKeyUp, comboSend, familyOf, guestKeys, toolsWant, toolsLine, toolsAutoStep, toolsAutoOn, toolsAutoFailed, TOOLS_TRIES, launchPending, discOffer, discCommands, discCanCopy, scaleFrame, shapeFrame, SCALES, deviceDots, cableOffered } from './model.js';
import { useAppsMode, appsMenuItems, guests, guestFocus, CLIP_HOLD_MS } from './appsmode.jsx';
import { ListHead, AppsSearch, RunningList, LaunchList, launchFirst } from './appslist.jsx';
import { readMeta, writeMeta, isProtectedMachine } from './meta.js';
import { defaultsOf } from './store.js';
import { ClipAskBar } from './clipask.jsx';
import { gatewayBar, gatewayOf } from './group.js';
import { KeysBtn, sendKeysItems } from './keys.jsx';
import { usePal, hexOf, has, Btn, IconBtn, IconMenuBtn, BarGroup, BarSep, icons, StateDot, TONE, overlayRoot, menus, vmRefusal, startPrep, needsPrep, soundDevItems } from './ui.jsx';
import { openLogs, openSnapshots, playAsks, openAsks } from './library.jsx';
import { opensOf, openStep, withoutAsks, askStale } from './openwith.js';
import { Configure } from './configure.jsx';
import { useAllDisplays } from './alldisplays.jsx';
import { Sheet, Pill } from './kit.jsx';
import { captureOffer, captureStep, captureEndedByServer, captureLabelKey, CAPTURE_IDLE, CAPTURE_NOTICE_MS } from './capture.js';

const BAR = 48;
// how often the Info menu's numbers are read
const INFO_MS = 2000;
// the window widths the machine's bar needs: all of it with the name and state, and compact (5.3)
const TOOLS_WIDE = 920, TOOLS_COMPACT = 660;
const now = () => (typeof lord !== 'undefined' && lord.now ? lord.now() : Date.now());
const THUMB_MS = 5000;
// dir → the LordOS tools' automatic update (model.js toolsAutoStep) for the machine's current run
const toolsRuns = new Map();
// what changes the screen (frames don't: the media node repaints by itself)
const jobKey = (j) => (j ? `${j.kind}.${j.status}.${j.pct}` : '');
const agKey = (g) => (g ? `${g.state}.${g.caps}.${g.version}.${g.system}.${g.tools}.${g.install}.${g.transfer}.${g.discMessage}` : '');
const sndKey = (x) => (x ? `${x.on}.${x.volume}.${x.microphone}.${x.mic}.${x.playing}.${x.recording}.${x.output}.${x.input}` : '');
const same = (a, b) => !!a && !!b && ['state', 'reason', 'code', 'detail', 'display', 'w', 'h', 'media', 'ended', 'lastError', 'stderr', 'cd', 'cdMessage', 'notice', 'noticeSeq', 'console', 'clipIn', 'clipBlockedRead', 'clipBlockedWrite', 'clipAsk', 'clipAskSeq', 'filesSeq', 'windowsSeq', 'iconsSeq', 'guestAppsSeq', 'closeRefusedSeq', 'captured', 'captureEnds', 'shell', 'guestLinkSeq', 'guestNotesSeq'].every((k) => a[k] === b[k]) && a.agent?.lost === b.agent?.lost
  && jobKey(a.job) === jobKey(b.job) && agKey(a.agent) === agKey(b.agent) && sndKey(a.sound) === sndKey(b.sound);

// the agent's state, first on the bar (mock "Session", 6.17): a light, and a popover with what it said and what works
function AgentButton({ ctl }) {
  const [open, setOpen] = useState(false);
  const ref = useRef(null);
  const g = ctl.st?.agent, cfg = ctl.cfg || {};
  // off while the machine is: nothing to wait for
  const tone = ctl.a.start ? 'off' : agentTone(g, cfg.agent);
  const label = !cfg.agent ? tr('machines.agent.off') : g?.state === 'connected' ? tr('machines.agent.connected') : ctl.a.start ? tr('machines.agent.idle') : tr('machines.agent.waiting');
  // the guest's LordOS tools are older than the ones this LordOS ships: a dot, and Update in the popover
  const old = toolsOutdated(g);
  return (
    <View ref={ref}>
      <IconBtn sym="agent" badge={TONE[tone]} dot={old ? '#0a84ff' : null} label={old ? `${label} · ${tr('machines.agent.toolsUpdate')}` : label} on={open} onClick={() => setOpen(!open)} />
      {open ? (
        <Overlay anchor={ref}>
          <View style={{ position: 'absolute', top: 30, left: -12, width: 340 }}>
            <AgentPop ctl={ctl} onClose={() => setOpen(false)} />
          </View>
        </Overlay>
      ) : null}
    </View>
  );
}
function AgentPop({ ctl, onClose }) {
  const p = usePal();
  const ref = useRef(null);
  const g = ctl.st?.agent, cfg = ctl.cfg || {};
  useEffect(() => {
    menus.open++;
    const a = addListener('pointerdown', (e) => { const n = ref.current; if (!n) return; const [lx, ly, w, h] = lord.getLayout(n.id); if (e.x < lx || e.x > lx + w || e.y < ly || e.y > ly + h) onClose(); });
    const b = addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); });
    return () => { menus.open--; a(); b(); };
  }, []);
  const on = g?.state === 'connected';
  const old = toolsOutdated(g);
  const tl = toolsLine(g);
  const lines = agentLines(g, cfg);
  const tone = ctl.a.start ? 'off' : agentTone(g, cfg.agent);
  const bg = p.dark ? '#34343c' : '#fbfbfd';
  const edge = p.dark ? 'rgba(255,255,255,0.14)' : 'rgba(0,0,0,0.12)';
  // the hypervisaur look (kit.jsx): a rounded popover with a nub toward the button, a light and the state in bold,
  // the system on a line of its own, then what works in this machine
  return (
    <View ref={ref}>
      <Image src={`svg:48:${hexOf(bg)}:asset:machines/symbols/nub-symbolic.svg`} style={{ marginLeft: 22, width: 22, height: 22, marginTop: -11, marginBottom: -1 }} />
      <View style={{ padding: 16, gap: 8, borderRadius: 16, backgroundColor: bg, border: `1px solid ${edge}`, boxShadow: '0 18px 44px rgba(0,0,0,0.45)' }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <StateDot tone={tone} size={10} />
          <Text style={{ flex: 1, fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{!cfg.agent ? tr('machines.agent.off') : on ? tr('machines.agent.connected') : tr('machines.agent.waiting')}</Text>
        </View>
        {/* the system and qemu-guest-agent's version, then the LordOS tools' own line: up to date, or which version
            this LordOS brings (and whether it's on its way) */}
        {on ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{[g.system, tr('machines.agent.version', { v: g.version })].filter(Boolean).join(' · ')}</Text>
          : <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{!cfg.agent ? tr('machines.agent.offHint') : ctl.st?.state === 'running' ? tr('machines.agent.installHint') : tr('machines.agent.bootHint')}</Text>}
        {on && tl ? <Text style={{ fontSize: 12, fontWeight: tl.key === 'old' ? 600 : 400, color: tl.key === 'old' ? '#0a84ff' : p.dim, textAlign: 'start' }}>
          {tl.key === 'old' ? tr(g.install === 1 ? 'machines.agent.toolsUpdating' : 'machines.agent.toolsOld', { have: tl.have, want: tl.want })
            : tr(tl.key === 'current' ? 'machines.agent.toolsCurrent' : 'machines.agent.toolsVersion', { v: tl.have })}</Text> : null}
        <View style={{ marginTop: 4, borderRadius: 12, backgroundColor: p.dark ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.035)', paddingVertical: 4 }}>
          {lines.map(([k, st], i) => (
            <View key={k} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 12, paddingVertical: 6,
              borderTopWidth: i ? 1 : 0, borderColor: p.dark ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.05)' }}>
              <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: st === 'works' ? TONE.run : 'transparent', border: st === 'works' ? 'none' : `1.5px solid ${p.dim}` }} />
              <Text style={{ flex: 1, fontSize: 12, color: st === 'works' ? p.text : p.dim, textAlign: 'start' }}>{tr(`machines.agent.cap.${k}`)}</Text>
              <Text style={{ fontSize: 11, fontWeight: 600, color: st === 'works' ? TONE.run : p.dim, textAlign: 'end' }}>{tr(`machines.agent.st.${st}`)}</Text>
            </View>
          ))}
        </View>
        {on ? (
          <View style={{ flexDirection: 'row', gap: 8, marginTop: 4, flexWrap: 'wrap' }}>
            <Pill label={g.install === 1 ? tr('machines.agent.installing') : old ? tr('machines.agent.updateTools') : guestWindows(g) ? tr('machines.agent.installTools') : tr('machines.agent.installHelper')}
              disabled={g.install === 1} onClick={() => lord.vm.agent(ctl.sid, 'install')} />
            <Pill label={tr('machines.agent.timesync')} onClick={() => lord.vm.agent(ctl.sid, 'timesync')} />
            {ctl.xfer.state === 'setup' || ctl.xfer.state === 'settingUp'
              ? <Pill label={ctl.xfer.state === 'settingUp' ? tr('machines.tb.transferSettingUp') : tr('machines.tb.transferSetup')} disabled={ctl.xfer.state === 'settingUp'} onClick={ctl.xfer.setup} /> : null}
          </View>
        ) : null}
        {/* a Linux guest without qemu-guest-agent (or one that doesn't let Install copy files): the LordOS Guest Tools
            disc, and once it is in, what to run in the machine (model.js discOffer) */}
        <DiscPanel ctl={ctl} g={g} cfg={cfg} mode={discOffer(g, cfg, ctl.st?.state === 'running', ctl.prot)} />
        {/* a second display in the guest that LordOS doesn't show (not on a PCI adapter): the tools turn it off when the
            person says so, and back on */}
        {on && (g.caps & AGENT_CAPS.extra) ? (
          <View style={{ gap: 6 }}>
            <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.extraHint')}</Text>
            <View style={{ flexDirection: 'row' }}><Pill label={tr('machines.agent.extraOff')} onClick={() => lord.vm.agent(ctl.sid, 'extraOff')} /></View>
          </View>
        ) : null}
        {on && (g.caps & AGENT_CAPS.extraOff) ? (
          <View style={{ flexDirection: 'row' }}><Pill label={tr('machines.agent.extraOn')} onClick={() => lord.vm.agent(ctl.sid, 'extraOn')} /></View>
        ) : null}
        {g?.install === 2 ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{guestWindows(g) ? tr('machines.agent.toolsInstalled') : tr('machines.agent.installed')}</Text> : null}
        {/* the clipboard is off in this machine's description (the default for machines made before 6.8): installing the
            helper again changes nothing, so say where it's turned on */}
        {on && (cfg.clipboard || 'off') === 'off' ? (
          <View style={{ gap: 6 }}>
            <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.clipOff')}</Text>
            <View style={{ flexDirection: 'row' }}><Pill label={tr('machines.tb.clipChange')} onClick={() => { onClose(); ctl.configure('sharing'); }} /></View>
          </View>
        ) : null}
        {/* the helper runs but the guest has no clipboard tool (a stock desktop without xclip or wl-clipboard): Install
            above puts it in through the guest's package manager, or the person installs it */}
        {clipNeedsTool(g, cfg) && g.install !== 1 ? <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.clipNoTool')}</Text> : null}
        {g?.install === 3 ? (() => {
          // lord-vm's words are English: a translated line, and its text only as a detail
          const e = installError(g.message);
          return <>
            <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.agent.err.${e.key}`)}</Text>
            {e.detail ? <Text style={{ fontSize: 11, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.err.detail', { text: e.detail })}</Text> : null}
          </>;
        })() : null}
      </View>
    </View>
  );
}

// The bar's device dots (5.3, deviceDots): one small dot per device the guest can use now, a pulled cable in amber;
// the dots together carry one tooltip that names each, so color is never the only sign
const DOT_COLOR = { net: '#7be3a4', netOut: '#ff9f0a', cd: '#7cc4e8', sound: '#7be3a4', mic: '#ff9f0a', folders: '#7be3a4', usb: '#7cc4e8', camera: '#ff9f0a' };
function DeviceDots({ dots }) {
  const label = dots.map((d) => tr(`machines.dot.${d.k}`)).join(' · ');
  return (
    <View title={label} style={{ flexDirection: 'row', alignItems: 'center', gap: 3, paddingHorizontal: 2, height: 28 }}>
      {dots.map((d) => <View key={d.k} style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: DOT_COLOR[d.k] || '#aaa' }} />)}
    </View>
  );
}

// The LordOS Guest Tools disc (docs/VIRTUALIZATION.md 6.17): 'insert' offers it; 'guide' says what to run in the machine,
// whose own prompt asks for the password (nothing is typed into the guest from here). The commands are shown to be
// typed; Copy only where the machine's clipboard can take them (discCanCopy). lord-vm takes the disc out by itself once
// the agent answers; "Take the Disc Out" does it now.
function DiscPanel({ ctl, g, cfg, mode }) {
  const p = usePal();
  if (!mode) return null;
  if (mode === 'insert') {
    return (
      <View style={{ gap: 6 }}>
        <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.discHint')}</Text>
        <View style={{ flexDirection: 'row' }}><Pill primary label={tr('machines.agent.discBtn')} onClick={() => lord.vm.agent(ctl.sid, 'disc')} /></View>
        {g?.discMessage ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr('machines.agent.discErr', { text: g.discMessage })}</Text> : null}
      </View>
    );
  }
  const copy = discCanCopy(g, cfg);
  return (
    <View style={{ gap: 6, padding: 10, borderRadius: 12, backgroundColor: p.dark ? 'rgba(10,132,255,0.12)' : 'rgba(10,132,255,0.08)' }}>
      <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.agent.discTitle')}</Text>
      <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.discStep')}</Text>
      {discCommands().map(({ key, cmd }) => (
        <View key={key} style={{ gap: 2 }}>
          <Text style={{ fontSize: 11, color: p.dim, textAlign: 'start' }}>{tr(`machines.agent.disc.${key}`)}</Text>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <Text style={{ flex: 1, fontFamily: 'monospace', fontSize: 12, color: p.text, textAlign: 'left', direction: 'ltr' }}>{cmd}</Text>
            {copy ? <Pill label={tr('machines.agent.discCopy')} onClick={() => clipboard.set(cmd)} /> : null}
          </View>
        </View>
      ))}
      <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.discPassword')}</Text>
      <View style={{ flexDirection: 'row' }}><Pill label={tr('machines.agent.discOut')} onClick={() => lord.vm.agent(ctl.sid, 'discOut')} /></View>
      {g?.discMessage ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr('machines.agent.discErr', { text: g.discMessage })}</Text> : null}
    </View>
  );
}

// The bar, in groups (5.3): the agent and what it brings (clipboard, folders, apps) | the guest's display and devices |
// shell, logs, screenshot, record | pause and power | configure and full screen. Icons, each named in its tooltip.
// apps: apps mode's panel (AppsController), the same buttons less the ones apps mode has no use for: the resolution (the
// guest's screen is this display's size then), Capture mouse (never in apps mode) and full screen (a panel)
function Tools({ ctl, floating, compact, tiny, apps }) {
  const { st, a, follow, cfg } = ctl;
  // the mock "Toolbar": sizes that fit this display, then Follow the window (5.3)
  const res = [
    ...resolutions(screen.width, screen.height, st?.w ? [st.w, st.h] : null).map(([w, h]) => ({ label: `${w} × ${h}`, checked: !follow && st?.w === w && st?.h === h, disabled: !a.keys, onClick: () => ctl.fixed(w, h) })),
    '-',
    { label: tr('machines.tb.follow'), checked: follow, onClick: () => ctl.setFollow(true) },
    { label: tr('machines.tb.sizesNote'), note: true },
    // the guest kept its size after Follow asked for another (its display driver doesn't follow): said, not hidden
    follow && ctl.stuck && st?.w ? { label: tr('machines.tb.followStuck', { w: st.w, h: st.h }), note: true } : null,
  ];
  const g = st?.agent;
  const clip = cfg?.clipboard || 'off';
  const clipOn = clip !== 'off';
  // the clipboard's directions are part of the machine's description: they change in Configure, with it off
  const clipItems = [
    ...['both', 'to-guest', 'from-guest', 'off'].map((x) => ({ label: tr(`machines.cfg.clip.${x}`), checked: clip === x, disabled: true })),
    '-',
    clipNeedsTool(g, cfg) ? { label: tr('machines.agent.st.needsXclip'), note: true } : null,
    g?.state === 'connected' && clipOn && !(g.caps & AGENT_CAPS.clipboard)
      ? { label: g.install === 1 ? tr('machines.agent.installing') : guestWindows(g) ? tr('machines.agent.installTools') : tr('machines.agent.installHelper'),
          disabled: g.install === 1, onClick: () => lord.vm.agent(ctl.sid, 'install') } : null,
    { label: tr('machines.tb.clipChange'), onClick: () => ctl.configure('sharing') },
  ];
  // shared folders (6.7): each one opens in Files; the mount command for a Linux guest is a click away
  const shares = cfg?.shares || [];
  // the transfer folder (6.7.1) first: open it here, where the machine has it, its setup in a Windows machine
  const xf = ctl.xfer;
  const xferItems = !cfg?.transfer ? [{ label: tr('machines.tb.transferOff'), onClick: () => ctl.configure('sharing') }, '-'] : [
    { label: tr('machines.tb.transferOpen'), onClick: xf.open },
    { label: xf.state === 'ready' ? (xf.windows ? tr('machines.win.transferDrive') : tr('machines.tb.transferLinux'))
      : xf.state === 'setup' ? tr('machines.tb.transferNeedsSetup') : xf.state === 'settingUp' ? tr('machines.tb.transferSettingUp') : tr('machines.tb.transferWaiting'), note: true },
    xf.state === 'setup' ? { label: tr('machines.tb.transferSetup'), onClick: xf.setup } : null,
    '-',
  ];
  const shareItems = [
    ...xferItems,
    ...shares.map((x) => ({ label: `${x.path.split('/').pop()}  ·  ${x.tag}`, hint: tr(x.writable ? 'machines.tb.shareRw' : 'machines.tb.shareRo'), onClick: () => openFolder(x.path) })),
    ...shares.map((x) => ({ label: tr('machines.tb.shareCopy', { tag: x.tag }), onClick: () => clipboard.set(mountCommand(x.tag)) })),
    shares.length ? '-' : null,
    !shares.length ? { label: tr('machines.tb.sharesNone'), note: true } : null,
    { label: shares.length ? tr('machines.tb.shareChange') : tr('machines.tb.shareAdd'), onClick: () => ctl.configure('sharing') },
  ];
  const resLabel = st?.w ? `${tr('machines.tb.resolution')}: ${st.w} × ${st.h}` : tr('machines.tb.resolution');
  return <>
    {!floating ? <>
      <BarGroup>
        <AgentButton ctl={ctl} />
        <IconMenuBtn sym="clipboard" label={`${tr('machines.cfg.clipboard')}: ${tr(`machines.cfg.clip.${clip}`)}`} items={clipItems} color={clipOn ? '#7be3a4' : null} />
        <IconMenuBtn sym="sharing" label={shares.length ? tr('machines.tb.foldersN', { n: shares.length }) : tr('machines.tb.folders')} items={shareItems} color={shares.length || xf.state === 'ready' ? '#7be3a4' : null} />
        {compact ? null : <IconBtn sym="apps" label={ctl.appsLabel} on={ctl.appsOn} disabled={!!ctl.appsBlocked && !ctl.appsOn} onClick={ctl.toggleApps} />}
      </BarGroup>
      {compact ? null : <BarSep />}
    </> : null}
    <BarGroup>
      {apps ? null : <IconMenuBtn sym="display" label={resLabel} items={res} disabled={!a.keys} on={!follow} />}
      {/* Capture mouse (6.25): Automatic (the tablet, the pointer free) or Captured; never in apps mode, which says why */}
      {apps ? null : <IconBtn sym="capture" label={tr(captureLabelKey({ on: ctl.capOn, why: ctl.capOffer.why }))} on={ctl.capOn}
        disabled={!ctl.capOffer.offered && !ctl.capOn} onClick={ctl.toggleCapture} />}
      {/* Keys: held keys, the guest's combinations, single keys and the on-screen keyboard (keys.jsx) */}
      <KeysBtn ctl={ctl} />
      <IconMenuBtn sym="eject" label={tr('machines.tb.devices')} items={ctl.devices} />
      {/* a dot for each device the guest can use now (5.3), its words in the tooltip */}
      {ctl.dots?.length && !compact ? <DeviceDots dots={ctl.dots} /> : null}
      {/* the machine's sound (6.9): its menu mutes it or turns it back on and picks the output and input; the microphone
          shows while the guest hears it */}
      <IconMenuBtn sym={ctl.heard ? 'sound' : 'sound-off'} label={tr('machines.tb.sound')} items={ctl.soundMenu} />
      {ctl.micLive ? <IconBtn sym="mic" color="#ff9f0a" label={tr('machines.tb.micLive')} onClick={() => ctl.configure('sound')} /> : null}
    </BarGroup>
    {compact ? null : <BarSep />}
    {/* the smallest windows: shell, logs, screenshot and record leave the bar (logs and screenshot are in Actions),
        unless a recording runs, so power, Configure and full screen still fit */}
    {tiny && !ctl.rec ? null : <BarGroup>
      {!floating ? <IconBtn sym="terminal" label={tr('machines.tb.shell')} on={ctl.shell} disabled={!ctl.shell && !ctl.a.reset} onClick={ctl.toggleShell} /> : null}
      <IconBtn sym="logs" label={tr('machines.cmd.logs')} onClick={ctl.logs} />
      <IconBtn sym="snapshot" label={tr('machines.tb.screenshot')} disabled={!a.keys || ctl.prot} onClick={ctl.screenshot} />
      {ctl.rec ? <IconBtn sym="stop" danger label={tr('machines.tb.recording', { t: ctl.recTime })} onClick={ctl.record} />
        : <IconBtn sym="record" color="#ff453a" label={tr('machines.tb.record')} disabled={!a.keys || ctl.prot} onClick={ctl.record} />}
    </BarGroup>}
    {ctl.rec ? <Text style={{ fontSize: 12, fontWeight: 600, color: '#ff6b6b', marginLeft: 4 }}>{ctl.recTime}</Text> : null}
    {compact ? null : <BarSep />}
    <BarGroup>
      {a.start ? <IconBtn sym="play" color="#30d158" label={st?.suspended || stateView(st?.state, st?.reason).key === 'suspended' ? tr('machines.cmd.resume') : tr('machines.start')} onClick={ctl.begin} />
        : <IconBtn sym={a.resume ? 'play' : 'pause'} label={a.resume ? tr('machines.cmd.resume') : tr('machines.cmd.pause')} disabled={!a.pause && !a.resume} onClick={() => ctl.cmd(a.resume ? 'resume' : 'pause')} />}
      <IconMenuBtn sym="power" label={tr('machines.cmd.power')} items={ctl.power} disabled={a.start} />
    </BarGroup>
    {compact ? null : <BarSep />}
    <BarGroup>
      {!floating ? <IconBtn sym="gear" label={tr('machines.cmd.configure')} onClick={() => ctl.configure()} /> : null}
      {apps ? null : floating ? <IconBtn sym="fullscreen-exit" label={tr('machines.cmd.exitFullscreen')} onClick={() => ctl.setFull(false)} />
        : <IconBtn sym="fullscreen" label={tr('machines.cmd.fullscreen')} onClick={() => ctl.setFull(true)} />}
    </BarGroup>
  </>;
}

function Header({ ctl, name }) {
  const p = usePal();
  const v = stateView(ctl.st?.state, ctl.st?.reason);
  // a small window: the name and state are in the window's title already; the tools lose their separators and the
  // placeholder for apps, so they still fit on the bar (the whole bar takes about 850 px with a short name, the
  // compact one about 610: below that the shell, logs, screenshot and record group goes too)
  const compact = useNarrow(TOOLS_WIDE);
  const tiny = useNarrow(TOOLS_COMPACT);
  return (
    <View style={{ height: BAR, flexDirection: 'row', alignItems: 'center', gap: compact ? 4 : 6, paddingHorizontal: compact ? 8 : 12, backgroundColor: p.raised }}>
      <StateDot tone={v.tone} size={9} />
      {compact ? null : <Text style={{ marginLeft: 4, fontSize: 13, fontWeight: 700, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', maxWidth: 260 }}>{name}</Text>}
      {compact ? null : <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap' }}>{tr(`machines.state.${v.key}`)}</Text>}
      <View style={{ flex: 1 }} />
      <Tools ctl={ctl} compact={compact} tiny={tiny} />
    </View>
  );
}

// full screen: a floating bar at the top edge, shown near it; a hint at the bottom for a few seconds
function FloatingBar({ ctl, name }) {
  const p = usePal();
  const [near, setNear] = useState(true);
  const [hint, setHint] = useState(true);
  // the bar stays while one of its menus is open (the pointer leaves it to reach the menu's items, 2026-11-20: the
  // resolution menu went away under the pointer) and hides once the menu closes with the pointer elsewhere
  const over = useRef(true);
  const leave = () => { over.current = false; if (!menus.open && !menus.panel) setNear(false); };
  useEffect(() => {
    const a = setTimeout(() => { if (!menus.open && !menus.panel) setNear(false); over.current = false; }, 2500), b = setTimeout(() => setHint(false), 5000);
    const c = setInterval(() => { if (!over.current && !menus.open && !menus.panel) setNear(false); }, 300);
    return () => { clearTimeout(a); clearTimeout(b); clearInterval(c); };
  }, []);
  const v = stateView(ctl.st?.state, ctl.st?.reason);
  return <>
    <View onMouseEnter={() => { over.current = true; setNear(true); }} style={{ position: 'absolute', left: 0, right: 0, top: 0, height: near ? 70 : 4, alignItems: 'center' }}>
      {near ? (
        <View onMouseEnter={() => { over.current = true; }} onMouseLeave={leave} style={{ marginTop: 10, height: 46, flexDirection: 'row', alignItems: 'center', gap: 4, paddingLeft: 14, paddingRight: 8,
          borderRadius: 14, backgroundColor: 'rgba(28,28,33,0.94)', boxShadow: '0 10px 28px rgba(0,0,0,0.45)' }}>
          <StateDot tone={v.tone} size={9} />
          <Text style={{ marginLeft: 6, marginRight: 10, fontSize: 13, fontWeight: 700, color: '#f2f2f5' }}>{name}</Text>
          <Tools ctl={ctl} floating />
        </View>
      ) : null}
    </View>
    {hint ? (
      <View style={{ position: 'absolute', left: 0, right: 0, bottom: 20, alignItems: 'center', pointerEvents: 'none' }}>
        <View style={{ paddingHorizontal: 14, paddingVertical: 8, borderRadius: 18, backgroundColor: 'rgba(0,0,0,0.68)' }}>
          <Text style={{ fontSize: 13, color: '#f2f2f5' }}>{tr('machines.fs.hint')}</Text>
        </View>
      </View>
    ) : null}
  </>;
}

// the card over the black area: starting, shutting down, no picture, lost display
function Card({ title, text, actions, width = 460, extra }) {
  const p = usePal();
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 16, paddingVertical: 12 }}>
      {/* never wider or taller than the window (apps mode's controller is small): its buttons wrap and stay in view */}
      <View style={{ width: '100%', maxWidth: width, maxHeight: '100%', padding: 20, gap: 8, borderRadius: 14, backgroundColor: p.bg, border: `1px solid ${p.line}` }}>
        <Text style={{ fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {text ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{text}</Text> : null}
        {extra}
        {actions?.length ? <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, justifyContent: 'flex-end', marginTop: 6 }}>{actions}</View> : null}
      </View>
    </View>
  );
}

// Off or suspended: one round Play in the middle (a player's, 5.3) on black. Only a suspended machine shows its last
// picture behind it, dimmed (it resumes to that); an off one doesn't (it was a shutdown or firmware screen, 2026-11-20)
function OffView({ dir, name, title, hint, label, onPlay, picture }) {
  const [hover, setHover] = useState(false);
  let src = '';
  if (picture) { try { const st = fs.stat(`${dir}/thumb.png`); src = thumbSrc(dir, st?.mtime || 0); } catch (e) { src = ''; } }
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: '#0c0c0f' }}>
      {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover', opacity: 0.35 }} /> : null}
      <View onClick={onPlay} title={label} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
        style={{ width: 96, height: 96, borderRadius: 48, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? 'rgba(10,132,255,0.92)' : 'rgba(0,0,0,0.55)',
          border: '2px solid rgba(255,255,255,0.9)', boxShadow: '0 12px 34px rgba(0,0,0,0.5)' }}>
        <Text style={{ fontSize: 40, color: '#fff', marginLeft: 7 }}>▶</Text>
      </View>
      <Text style={{ marginTop: 18, fontSize: 17, fontWeight: 700, color: '#f2f2f5' }}>{name}</Text>
      <Text style={{ marginTop: 4, fontSize: 13, color: 'rgba(242,242,245,0.75)' }}>{title}</Text>
      {hint ? <Text style={{ marginTop: 2, maxWidth: 420, fontSize: 12, color: 'rgba(242,242,245,0.6)', textAlign: 'center' }}>{hint}</Text> : null}
    </View>
  );
}

// BootFailed: why (a cause QEMU's own words name, model.js failCause), a fix that fits it (start with less memory, go
// back to a snapshot: failActions), Start Again, Settings, and the machine's log
function Failed({ name, v, st, log, fixes, onStart, onSettings, onAnyway, onDiscard, onUse2d, onLess, onSnapshot, busy }) {
  const p = usePal();
  const neverRan = v.why === 'start-failed';
  const cause = failCause(v.why, log);
  const lines = (log || '').split('\n').filter(Boolean).slice(-60);
  const bad = (l) => /fail|error|refus|crash|exited|killed|cannot|can't|panic/i.test(l);
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, padding: 22, gap: 12, backgroundColor: p.bg }}>
      <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{neverRan ? tr('machines.fail.didntStart', { name }) : tr('machines.fail.stopped', { name })}</Text>
      <Text style={{ fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr(`machines.why.${cause}`)} {neverRan ? tr('machines.fail.diskSafe') : ''}</Text>
      <View style={{ paddingHorizontal: 14, paddingVertical: 12, borderRadius: 12, backgroundColor: p.badBg }}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{`${tr('machines.fail.likely')} ${tr(`machines.fix.${cause}`, { code: st?.code })}`}</Text>
        {st?.detail ? <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start', direction: 'ltr' }}>{st.detail}</Text> : null}
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
        {offers2d(v.why) ? <Btn big primary label={tr('machines.fail.use2d')} disabled={busy} onClick={onUse2d} /> : null}
        {(fixes || []).map((f) => (f.k === 'less'
          ? <Btn key="less" big primary label={tr('machines.budget.less', { mb: f.mb })} disabled={busy} onClick={() => onLess(f.mb)} />
          : <Btn key="snap" big label={tr('machines.fail.snapshot')} disabled={busy} onClick={onSnapshot} />))}
        <Btn big primary={!offers2d(v.why) && !(fixes || []).some((f) => f.k === 'less')} label={tr('machines.fail.startAgain')} disabled={busy} onClick={onStart} />
        {v.why === 'resume-failed' ? <>
          <Btn big label={tr('machines.fail.resumeAnyway')} onClick={onAnyway} />
          <Btn big danger label={tr('machines.fail.discardState')} onClick={onDiscard} />
        </> : <Btn big label={tr('machines.cmd.configure')} onClick={onSettings} />}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.fail.log')}</Text>
        <Btn label={tr('machines.logs.copy')} onClick={() => clipboard.set(log || '')} />
      </View>
      <ScrollView style={{ flex: 1, borderRadius: 12, backgroundColor: p.dark ? '#16171b' : 'rgba(0,0,0,0.04)' }} contentStyle={{ paddingHorizontal: 14, paddingVertical: 12 }}>
        {lines.map((l, i) => <Text key={i} style={{ fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: bad(l) ? '#ff9a9a' : p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{l}</Text>)}
      </ScrollView>
    </View>
  );
}

// Apps mode's controller (7.2): what's left of the machine's window while its apps are windows of their own, as a panel
// (like a Unity mode's launcher): Running, the guest's windows (a click brings one to the front, × asks the guest to close
// it), then the guest's apps to open, with a search; and the way back to its desktop (Esc too). The same lists are in the
// machine's status item by the clock (appsstatus.jsx); both come from appslist.jsx.
// The machine's bar is on the panel too (the agent, clipboard, folders and apps; keys, devices and sound; the console,
// logs, screenshot and recording; pause and power; Configure), its groups wrapped over as many rows as the panel needs.
function AppsController({ name, g, onLeave, ctl }) {
  const p = usePal();
  const [query, setQuery] = useState('');
  const apps = launcherApps(g?.apps || [], query, g?.appIcons);
  const v = stateView(ctl?.st?.state, ctl?.st?.reason);
  return (
    <View style={{ flex: 1, padding: 16, gap: 10, backgroundColor: p.bg }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <StateDot tone={v.tone} size={9} />
        <Text style={{ flex: 1, minWidth: 0, fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{name}</Text>
        <Btn label={tr('machines.apps.leave')} onClick={onLeave} />
      </View>
      {ctl ? <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6 }}><Tools ctl={ctl} compact apps /></View> : null}
      <ListHead label={tr('machines.apps.running')} />
      <ScrollView style={{ maxHeight: 176, borderRadius: 10, backgroundColor: p.raised }} contentStyle={{ padding: 4 }}>
        <RunningList g={g} />
      </ScrollView>
      <ListHead label={tr('machines.apps.launch')} />
      <AppsSearch value={query} onChange={setQuery} onSubmit={() => launchFirst(g, apps)} onCancel={onLeave} />
      <ScrollView style={{ flex: 1, borderRadius: 10, backgroundColor: p.raised }} contentStyle={{ padding: 4 }}>
        <LaunchList g={g} apps={apps} />
      </ScrollView>
    </View>
  );
}

// Picture in Picture's one control (5.4): the machine's name and "Back to Window", shown while the pointer is over the
// view; a double click on the picture does the same. Nothing else: the view takes no keys and no pointer for the guest.
function PipBack({ name, onBack }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, justifyContent: 'flex-start', padding: 8 }}>
      {hover ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, height: 30, borderRadius: 15, alignSelf: 'flex-start', backgroundColor: 'rgba(0,0,0,0.62)' }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: '#f2f2f5', whiteSpace: 'nowrap', overflow: 'hidden', maxWidth: 200, textAlign: 'start' }}>{name}</Text>
          <View onMouseDown={(e) => { e.stopPropagation?.(); onBack(); }} title={tr('machines.view.pipBack')}
            style={{ height: 22, paddingHorizontal: 10, borderRadius: 11, justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.18)' }}>
            <Text style={{ fontSize: 12, color: '#fff', whiteSpace: 'nowrap' }}>{tr('machines.view.pipBack')}</Text>
          </View>
        </View>
      ) : null}
    </View>
  );
}

// machines whose bridged network the admin helper is making now (dir): one start at a time, whatever remounts
const bridging = new Set();
// how long a start waits for a machine suspended to make room for it (3.7)
const BUDGET_WAIT_MS = 120000;

export function MachineWindow({ dir, name: name0, loadvm, followed }) {
  const p = usePal();
  const { id: winId, focused, mode } = useAppWindow();
  // Picture in Picture (5.4): the picture only, floating; no input reaches the guest from it
  const pip = mode === 'pip';
  // full screen moves the app in the tree and remounts it: the session belongs to the window
  const [sid, setSid] = useWindowState('vm.sid', 0);
  const [st, setSt] = useWindowState('vm.state', null);
  const [err, setErr] = useWindowState('vm.err', '');
  // a start on its way: a bridged machine's network is made by the admin helper first (6.5), shown as Starting
  const [starting, setStarting] = useWindowState('vm.starting', false);
  // the machine's graphics card (machine.json's gpu): like 3D, it can't be suspended or keep snapshots with its memory
  const [gpu, setGpu] = useWindowState('vm.gpu', () => (has() ? lord.vm.list().find((x) => x.dir === dir)?.gpu || '' : ''));
  const gpuRef = useRef(gpu);
  gpuRef.current = gpu;
  // a protected machine (the anonymity profile, model.js machineProtected): its window is protected and locked (a neutral
  // fill in every capture of the screen), and lord.vm.protect keeps its picture from leaving as a picture of its own
  const [prot] = useWindowState('vm.prot', () => has() && isProtectedMachine(dir));
  const protRef = useRef(prot);
  protRef.current = prot;
  useEffect(() => { if (prot) wm.setProtected(winId, true, { lock: true }); }, [prot, winId]);
  useEffect(() => { if (prot && sid) lord.vm.protect(sid); }, [prot, sid]);
  const [closing, setClosing] = useWindowState('vm.closing', false);   // false, 'shutdown' or 'suspend'
  const [askClose, setAskClose] = useWindowState('vm.ask', false);
  const [always, setAlways] = useState(false);
  const [follow, setFollow] = useWindowState('vm.follow', true);
  // apps mode (7.2): the person's choice, kept per machine (app.json), on once the guest's helper lists its windows
  const [appsOn, setAppsOn] = useWindowState('vm.apps', () => !!readMeta(dir).appsMode);
  const appsRef = useRef(false);
  const appsBlockedRef = useRef(null);
  const preApps = useRef(null);
  const [log, setLog] = useState('');
  const [name, setName] = useState(name0);
  const [iso, setIso] = useState('');
  const [pickCd, setPickCd] = useState(false);
  const [cdWant, setCdWant] = useState(null);       // the disc asked for while lord-vm changes it
  const [cdErr, setCdErr] = useState('');
  const snapJobs = useRef([]);
  // the Shell (6.18): the guest's serial console in a terminal pane under the picture; the keyboard goes to whichever
  // of the two was clicked last
  const [shell, setShell] = useWindowState('vm.shell', false);
  const [cfgSheet, setCfgSheet] = useState(null);     // Configure as a sheet over the machine's picture
  const [shellKeys, setShellKeys] = useState(false);
  // which shell the pane shows: the agent's (the signed-in person's own shell in the guest, 6.18, with the LordOS tools 8
  // or w12) or the serial console; the agent's when the tools have it, unless the person picked the console
  const [shellSrc, setShellSrc] = useWindowState('vm.shellSrc', 'agent');
  const agentShell = !!(st?.agent?.state === 'connected' && st.agent.caps & AGENT_CAPS.shell);
  const shellUse = shellSrc === 'agent' && agentShell ? 'agent' : 'serial';
  const shellTerm = shellUse === 'agent' ? st?.shell : st?.console;
  // the clipboard's "ask before each paste" bar (model.js clipAsk*): while it shows, the keyboard is the bar's
  const [askSt, setAskSt] = useState(clipAskEmpty);
  const askKind = clipAskShown(askSt);
  const shellRef = useRef(null);
  const [rec, setRec] = useWindowState('vm.rec', null);       // { id, at } while the picture is recorded
  const [, tick] = useState(0);
  useEffect(() => { if (!rec) return undefined; const t2 = setInterval(() => tick((x) => x + 1), 1000); return () => clearInterval(t2); }, [rec]);
  const box = useRef(null);
  const root = useRef(null);
  const sized = useRef({ last: null, want: null, at: 0, rate: false, thumb: 0, asked: null, askedAt: 0 });
  const [stuck, setStuck] = useState(false);
  // files dropped on the window, on their way into the guest (lord.vm.files): the ones still copying
  const [copies, setCopies] = useState([]);
  const filesSeen = useRef(-1);
  const onFiles = useRef(null);
  const buttons = useRef(new Set());
  // the Keys panel's held keys (model.js latch*): down in the guest until the next key or combination; let go of with
  // everything else whenever the window stops sending the keyboard (blur, leaving, the machine stopping)
  const latchRef = useRef([]);
  const [latched, setLatched] = useState([]);
  const keysTo = (id, r) => { latchRef.current = r.latched; setLatched(r.latched); if (id) r.send.forEach(([c, v]) => lord.vm.key(id, c, v)); };
  const dropLatched = () => { if (latchRef.current.length) { latchRef.current = []; setLatched([]); } };
  const wheel = useRef(0);
  const sidRef = useRef(sid);
  sidRef.current = sid;
  const closingRef = useRef(closing);
  closingRef.current = closing;
  const followRef = useRef(follow);
  const shapeRef = useRef(false);
  followRef.current = follow;
  overlayRoot.current = root.current;

  // the start itself, from the current render (the admin helper's answer may come after a remount)
  const startNow = useRef(null);
  startNow.current = (active, opts) => {
    setStarting(false);
    const id = active ? lord.vm.attach(dir) : lord.vm.start(dir, opts || {});
    if (!id) { setErr(active ? tr('machines.win.cantAttach') : vmRefusal(tr('machines.win.cantStart'))); return; }
    if (!active) toolsRuns.delete(dir);            // a new run: the tools' automatic update starts over
    sized.current = { last: null, want: null, at: 0, rate: false, thumb: 0, asked: null, askedAt: 0 };
    setErr(''); setSt(null); setSid(id);
  };
  const begin = (opts) => {
    if (starting || bridging.has(dir)) return;
    if (!has()) { setErr(tr('machines.noBackend')); return; }
    const m = lord.vm.list().find((x) => x.dir === dir);
    if (!m) { setErr(tr('machines.win.gone')); return; }
    // a description this LordOS can't read (a newer one wrote it, or it's damaged): never started
    if (unreadableOf(m) && !stateView(m.state, m.reason).active) { setName(m.name); setErr(tr(`machines.unreadable.${unreadableOf(m)}Hint`, { name: m.name })); return; }
    setName(m.name); setIso(m.iso || '');
    setLog('');
    const active = stateView(m.state, m.reason).active;
    // the resource budget (3.7): lord-server decides it for every start (lord.vm.budget, the same check lord.vm.start
    // makes); when it doesn't fit, the window offers to suspend a running machine or to start with less memory
    if (!active && lord.vm.budget) {
      const b = lord.vm.budget(dir);
      if (b && !b.ok) { setOver({ b, m, plan: budgetPlan(b, lord.vm.list(), m), waiting: '' }); return; }
    }
    setOver(null);
    // a bridged machine's network first (6.5): Starting meanwhile, then the start, once
    setGpu(m.gpu || '');
    if (!active && needsPrep(m)) {
      bridging.add(dir);
      setErr('');
      setStarting(true);
      // a window closed meanwhile doesn't start its machine after all
      startPrep(m).then(() => { bridging.delete(dir); if (windows.get().some((w) => w.id === winId)) startNow.current(false, opts); },
        (e) => { bridging.delete(dir); setStarting(false); setErr(e.message); });
    } else startNow.current(active, opts);
  };
  const beginRef = useRef(begin);
  beginRef.current = begin;
  // a start the budget refused: { b, m, plan, waiting } (waiting: the name of the machine being suspended for it)
  const [over, setOver] = useState(null);
  useEffect(() => {
    if (!over?.waiting) return undefined;
    // the start goes on once the suspend freed enough; a suspend that fails or takes longer than BUDGET_WAIT_MS says so
    const until = Date.now() + BUDGET_WAIT_MS;
    const timer = setInterval(() => {
      const b = lord.vm.budget(dir);
      if (b?.ok) { clearInterval(timer); setOver(null); beginRef.current(); } else if (Date.now() > until) { clearInterval(timer); setOver(null); setErr(tr('machines.budget.stillShort', { name: over.waiting })); }
    }, 1000);
    return () => clearInterval(timer);
  }, [over?.waiting]);
  const suspendFor = (x) => { lord.vm.stop(x.dir, 'suspend'); setOver({ ...over, waiting: x.name }); };
  const startWith = (mb) => {
    const id = has() ? lord.vm.set(dir, { memoryMb: mb }) : 0;
    setOver(null);
    if (!id) setErr(vmRefusal(tr('machines.cfg.refused'))); else setTo2d(id);
  };
  // QEMU's 3D display aborted (why 'qemu-3d-crash', QEMU issue 4918): Graphics 2D for this machine (lord-vm --set, a
  // job), then it starts again; Start with less memory (the budget, the failure page) is the same kind of change
  const [to2d, setTo2d] = useState(0);
  useEffect(() => {
    if (!to2d) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(to2d);
      if (!r || !r.done) return;
      setTo2d(0);
      if (r.ok) beginRef.current(); else setErr(tr('machines.cfg.refused'));
    }, 300);
    return () => clearInterval(timer);
  }, [to2d]);
  const use2d = () => {
    const id = has() ? lord.vm.set(dir, { graphics: '2d' }) : 0;
    if (!id) setErr(vmRefusal(tr('machines.cfg.refused'))); else setTo2d(id);
  };
  // Play on the library's card while this window is open: the machine starts if it's off here
  const asked = useStore(playAsks, (x) => x[dir] || 0);
  const askedSeen = useRef(asked);
  useEffect(() => {
    if (asked === askedSeen.current) return;
    askedSeen.current = asked;
    if (playStarts(sid, starting || bridging.has(dir), closing) && !askClose) { setErr(''); begin(); }
  }, [asked]);
  // a window opened to go back to a snapshot (with memory, the machine off) starts from it
  // (not when the machine was renamed with its folder and this window only follows it there)
  useEffect(() => { if (!followed && !sid && !st && !err) begin(loadvm ? { loadvm } : undefined); }, []);
  const stRef = useRef(st);
  stRef.current = st;
  // the window went away (not a remount): let go of the machine; one this window started is shut down by lord-vm
  useEffect(() => () => { if (sid && !windows.get().some((w) => w.id === winId)) lord.vm.close(sid); }, [sid]);

  // How often the machine's state is read: lord.vm.poll is cheap, but every read wakes lord-server's loop. 50 ms while
  // something is quick to change (a resize being settled, starting, stopping, closing), 100 ms otherwise, 500 ms while no
  // window shows the guest (minimized, another desktop, covered); shown again, at once. It was 16 ms: 60 wakes a second
  // for as long as a machine ran, hidden or not.
  const hiddenRef = useRef(false);
  const kickRef = useRef(null);
  useEffect(() => {
    if (!sid) return undefined;
    let timer = 0, gone = false;
    const next = (q) => {
      if (gone) return;
      const z = sized.current;
      const busy = !q || z.want || q.state !== 'running' || closingRef.current;
      timer = setTimeout(tick, hiddenRef.current ? (q ? 500 : 100) : busy ? 50 : 100);
    };
    const tick = () => {
      timer = 0;
      const q = lord.vm.poll(sid);
      if (!q) { next(null); return; }
      if (pollOnce(q) !== false) next(q);
    };
    kickRef.current = () => { if (timer) { clearTimeout(timer); tick(); } };
    const pollOnce = (q) => {
      setSt((prev) => (same(prev, q) ? prev : q));
      snapJobs.current = snapJobs.current.filter((j) => { const r = lord.vm.job(j); if (r?.done && !r.ok) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.snap.failed'), body: r.out.trim().split('\n').pop() || '', sound: false }); return r && !r.done; });
      if (q.cd === 2 || q.cd === 3) setCdWant((w) => { if (w !== null) { if (q.cd === 2) setIso(w); else setCdErr(q.cdMessage || '?'); } return null; });
      const z = sized.current;
      if (q.display === 'up' && !z.rate) { z.rate = true; try { lord.vm.rate(sid, refreshRate(lord.display.list())); } catch (e) { lord.vm.rate(sid, 60); } }
      // the window's size, to the guest after it stayed the same for 400 ms (unless a fixed resolution was chosen)
      if (q.display === 'up' && box.current && followRef.current && !appsRef.current) {
        const want = guestSize(lord.getLayout(box.current.id), z.last);
        if (!want) z.want = null;
        else if (!z.want || z.want[0] !== want[0] || z.want[1] !== want[1]) { z.want = want; z.at = now(); }
        else if (now() - z.at > 400) { lord.vm.resize(sid, want[0], want[1]); z.last = want; z.want = null; z.asked = want; z.askedAt = now(); }
      }
      // a fixed resolution: after the person resized the window (the frame the same for 400 ms), it takes the guest's
      // shape again, keeping its width (shapeFrame); tried once per frame size, so a window that can't go there isn't fought.
      // Only the person's resize starts it (the frame changed, and not to the size set here): a guest that keeps changing
      // its own display size never moves this window
      if (q.display === 'up' && box.current && shapeRef.current && q.w > 0 && q.h > 0) {
        const w0 = windows.get().find((x) => x.id === winId);
        const f0 = w0 ? wm.frame(w0) : null, b0 = lord.getLayout(box.current.id);
        const key = f0 ? `${f0.w}x${f0.h}` : '';
        if (key && z.shapeKey === undefined) z.shapeKey = key;                 // the frame it opened with: not a resize
        else if (key && key !== z.shapeKey) { z.shapeKey = key; z.shapeAt = now(); z.shapeDone = key === z.shapeSet; }
        else if (key && !z.shapeDone && now() - z.shapeAt > 400) {
          z.shapeDone = true;
          const s = shapeFrame(f0, { w: b0[2], h: b0[3] }, q.w, q.h, workAreaOf(displayFor(f0)));
          if (s) { z.shapeSet = `${s.w}x${s.h}`; wm.resize(winId, s.w, s.h); }
        }
      }
      // asked for a size the guest didn't take (Follow the window): the resolution menu says so
      const stuckNow = q.display === 'up' && followRef.current && !appsRef.current && followStuck(z.asked, [q.w, q.h], z.askedAt, now());
      setStuck((x) => (x === stuckNow ? x : stuckNow));
      if (q.filesSeq !== filesSeen.current || (q.ended && filesSeen.current !== -2)) { filesSeen.current = q.ended ? -2 : q.filesSeq; onFiles.current?.(lord.vm.files(sid)); }
      // the card picture in the library, every few seconds while it runs
      // only while the guest's system is up (its agent answers, when it has one): never its firmware, boot or shutdown
      // screens, which a card would otherwise keep
      const sysUp = !cfgRef.current?.agent || q.agent?.state === 'connected';
      // (never a protected machine's: no picture of it stays on disk)
      if (!protRef.current && q.display === 'up' && q.state === 'running' && sysUp && !closingRef.current && now() - z.thumb > THUMB_MS) { z.thumb = now(); lord.vm.snapshot(sid); }
      if (q.ended) {
        lord.vm.close(sid);
        setSid(0);
        toolsRuns.delete(dir);                     // the run ended: the tools' automatic update starts over next time
        if (closingRef.current) { setClosing(false); wm.close(winId, true); return false; }
        // an unattended install ended with the installer's restart: lord-vm took the installer and the seed disk away,
        // so starting again is the installed system (3.9, "Unattended install")
        // With "Fresh install" asked in the assistant (app.json freshSnap), the machine, off and installed, gets that
        // snapshot first (5.2), once.
        if (q.state === 'stopped' && q.reason === 'installed') {
          if (!readMeta(dir).freshSnap) { setTimeout(() => begin(), 300); return false; }
          writeMeta(dir, { freshSnap: false });
          const r = lord.vm.snapTake(dir, tr('machines.snap.fresh'));
          const wait = () => { const j = r?.job ? lord.vm.job(r.job) : null; if (j && !j.done) { setTimeout(wait, 400); return; } begin(); };
          setTimeout(wait, 300);
          return false;
        }
        // the guest restarted and QEMU couldn't do it in place, so Machines starts it again: a machine with an Intel
        // graphics card (a reset crashes a GVT-g guest, so QEMU ends on one; docs/MACHINES-GPU.md, Mode A), or one with
        // 3D graphics whose QEMU aborted at the guest's restart (QEMU issue 4918; lord-vm says 'reset' only when the
        // guest's tools said it restarts)
        if (restartsAgain(q.state, q.reason)) { setTimeout(() => begin(), 300); return false; }
        if (q.state !== 'stopped' || !['guest-shutdown', 'host-shutdown', 'poweroff', 'installed'].includes(q.reason)) setLog(lord.vm.log(dir, 'vm', 16384));
        return false;
      }
      return true;
    };
    timer = setTimeout(tick, 0);
    return () => { gone = true; kickRef.current = null; if (timer) clearTimeout(timer); };
  }, [sid]);

  // whether any window shows the guest's picture (this one, or one of its apps mode windows): when none does, lord-server
  // stops reading its frames and takes them less often (server/src/vm/vmpace.h); shown again, at once
  const sightIds = useStore(windows, (ws) => [winId, ...ws.filter((w) => w.args?.guestWin?.dir === dir).map((w) => w.id)].join('\n'));
  const outOfSight = useWindowsOutOfSight(sightIds.split('\n'));
  useEffect(() => {
    hiddenRef.current = outOfSight;
    if (sid && typeof lord.vm.shown === 'function') lord.vm.shown(sid, !outOfSight);
    if (!outOfSight) kickRef.current?.();      // shown again: its state at once, then the visible pace
  }, [sid, outOfSight]);

  // Closing the window (5.3): Suspend, Shut Down or Keep Running, asked unless the machine's Configure says which. A
  // shutdown resumes a paused machine first, so it can hear the power button; Keep Running tells lord-vm to stay when
  // this window lets go of it.
  const finish = (how) => {
    const s = sidRef.current;
    setAskClose(false);
    if (!s) { wm.close(winId, true); return; }
    if (how === 'keep') { lord.vm.cmd(s, 'keep'); lord.vm.close(s); setSid(0); wm.close(winId, true); return; }
    setClosing(how);
    if (how === 'suspend') { lord.vm.cmd(s, 'suspend'); return; }
    if (stRef.current?.state === 'paused') lord.vm.cmd(s, 'resume');
    lord.vm.cmd(s, 'shutdown');
  };
  useEffect(() => wm.setCloseGuard(winId, () => {
    const s = sidRef.current;
    const q = stRef.current;
    if (!s || !q || !['running', 'paused', 'starting', 'stopping'].includes(q.state)) return true;
    if (closingRef.current) return false;
    // a machine with 3D (virgl can't be saved) or a graphics card can't be suspended: it shuts down instead
    const how = closeChoice(readMeta(dir)) === 'suspend' && noSuspend(q, gpuRef.current) ? 'shutdown' : closeChoice(readMeta(dir));
    if (how === 'ask' || q.state === 'starting') setAskClose(true); else finish(how);
    return false;
  }), [winId]);

  // a Whonix group's Workstation (docs/VIRTUALIZATION.md 19.8, 19.9): while its Gateway is down it has no network at all
  // (the link is a pulled cable), and a bar says so with Start Gateway; the session links the two once both run
  const inGroup = /\.lordgroup\/[^/]+\.lordvm$/.test(dir);
  const [gwBar, setGwBar] = useState(null);           // { bar: null | 'off' | 'starting', gw }
  const [gwAsked, setGwAsked] = useState(0);
  useEffect(() => {
    if (!inGroup || !has()) return undefined;
    const look = () => {
      try {
        const list = lord.vm.list();
        const ws = list.find((x) => x.dir === dir), gw = gatewayOf(ws, list);
        const bar = gatewayBar(ws, gw);
        setGwBar((o) => (o?.bar === bar && o?.gw?.state === gw?.state ? o : { bar, gw }));
      } catch (e) { /* the list isn't there: no bar */ }
    };
    look();
    const timer = setInterval(look, 2000);
    return () => clearInterval(timer);
  }, [dir, gwAsked]);
  const startGateway = () => {
    const gw = gwBar?.gw;
    if (!gw) return;
    setGwBar({ bar: 'starting', gw });
    startPrep(gw).then(() => { lord.vm.startDetached(gw.dir); setGwAsked((n) => n + 1); }, () => setGwAsked((n) => n + 1));
  };

  // lord-vm's notices (VM_NOTICE): the network going and coming back (6.5), a Clone's new identity (6.3), a shared folder
  // gone because its virtiofsd ended (6.7: back at the next start); the good news goes away by itself
  const [noticeGone, setNoticeGone] = useState(0);
  useEffect(() => {
    if (!st?.noticeSeq || !['network-back', 'identity'].includes(st.notice)) return undefined;
    const seq = st.noticeSeq;
    const timer = setTimeout(() => setNoticeGone(seq), 8000);
    return () => clearTimeout(timer);
  }, [st?.noticeSeq]);
  const noticeOn = !!st?.notice && st.noticeSeq !== noticeGone;
  const noticeText = !st?.notice ? '' : st.notice === 'recovered' ? tr('machines.win.recovered')
    : st.notice === 'network-back' ? tr('machines.win.netBack', { s: (Math.max(0, st.noticeCode || 0) / 1000).toFixed(1) })
    : st.notice === 'identity' ? tr(st.noticeCode === 1 ? 'machines.win.identityName' : 'machines.win.identityDone')
    : st.notice === 'share-lost' ? (st.noticeCode === 1 ? tr('machines.win.transferLost') : tr('machines.win.shareLost', { folder: st.noticeText || '' }))
    : /reconnecting/.test(st.noticeText || '') ? tr('machines.win.netReconnecting') : tr('machines.win.netLost');
  const v = startPhase(sid, st, starting) === 'starting' && st ? stateView('starting') : stateView(st?.state, st?.reason);
  const a0 = actionsFor(sid ? st?.state : 'stopped');
  const a = starting && !sid ? { ...a0, start: false } : a0;     // no second Play while the network is being made
  useEffect(() => { wm.setTitle(winId, windowTitle(name || dir, err ? tr('machines.state.crashed') : tr(`machines.state.${v.key}`))); }, [name, v.key, err]);

  // keyboard while in front; everything held is let go of on blur
  const live = !!sid && st?.display === 'up' && (st.state === 'running' || st.state === 'paused') && !closing && !askClose && !(shell && shellKeys) && !pickCd && !appsRef.current && !askKind && !pip;
  // the console's stream, asked for once the pane is open on a running machine
  useEffect(() => { if (shell && shellUse === 'serial' && sid && !st?.console && (st?.state === 'running' || st?.state === 'paused')) lord.vm.console(sid); }, [shell, shellUse, sid, st?.state, !!st?.console]);
  // the agent's shell, asked for once the pane shows it (its size follows the terminal by itself)
  useEffect(() => { if (shell && shellUse === 'agent' && sid && !st?.shell && st?.state === 'running') lord.vm.shell(sid, 100, 14); }, [shell, shellUse, sid, st?.state, !!st?.shell]);
  useEffect(() => {
    if (!shell || !shellKeys || !focused || !shellTerm) return undefined;
    const id = shellTerm;
    return addListener('keydown', (e) => {
      // the session's every key reaches this listener: only the ones typed while this window has the keyboard, now
      if (menus.open > 0 || !wm.hasKeyboard(winId)) return;
      const key = e.key && e.key.length > 1 ? e.key : '';
      const base = e.ctrl || e.alt ? e.base || '' : '';
      if (!key && !e.char && !base) return;
      e.preventDefault?.();                      // Tab goes to the machine, not to the window's buttons (sdk/focus.js)
      term.key(id, key, e.char || '', base, e.mods & 7);
    });
  }, [shell, shellKeys, focused, shellTerm]);
  // notifications from the guest (6.12, Configure's switch): shown as from the machine, its name first, never more
  // than lord-vm and lord-server let through
  useEffect(() => {
    if (!sid || !st?.guestNotesSeq) return;
    for (const n of lord.vm.guestNotes(sid) || []) notify({ app: 'machines', label: tr('machines.note.from', { name: name || '' }), title: n.title, body: n.body, sound: false });
  }, [sid, st?.guestNotesSeq]);
  // a link from the guest (6.12): a bar the person answers; Open works only after the bar has been seen a moment
  const linkSeq = st?.guestLinkSeq || 0;
  const [linkReady, setLinkReady] = useState(0);
  useEffect(() => { if (!linkSeq) return undefined; const t = setTimeout(() => setLinkReady(linkSeq), 800); return () => clearTimeout(t); }, [linkSeq]);
  const linkAnswer = (open) => { if (sid && linkSeq) lord.vm.guestLinkAnswer(sid, linkSeq, open); };
  useEffect(() => {
    if (!live || !focused) { if (sid) { lord.vm.release(sid); buttons.current.clear(); } dropLatched(); return undefined; }
    // a key pressed while one of the window's menus is open is the menu's, down and up (Esc closes it)
    const kept = new Set();
    // each key asks whether this window has the keyboard at that moment (wm.hasKeyboard: in front, no search, sheet or
    // system prompt open), not the focus flag of the last render: a key typed in another window never reaches the
    // guest, and the first one lets go of everything held there, the Keys panel's held keys included (model.js guestKeys)
    const gk = guestKeys({ send: (c, v) => lord.vm.key(sid, c, v), owns: () => wm.hasKeyboard(winId), release: () => { lord.vm.release(sid); dropLatched(); } });
    const ka = addListener('keydown', (e) => {
      if (!(e.code > 0)) return;
      // ❖0 opens the library (index.jsx), never reaches the machine
      if (menus.open > 0 || kept.has(e.code) || (e.meta && !e.ctrl && !e.alt && (e.base || e.key) === '0')) { kept.add(e.code); return; }
      if (!gk.down(e.code)) return;
      e.preventDefault?.();                      // Tab goes to the machine, not to the window's buttons (sdk/focus.js)
    });
    const kb = addListener('keyup', (e) => {
      if (!(e.code > 0)) return;
      if (kept.delete(e.code)) return;
      if (gk.up(e.code) && latchRef.current.length) keysTo(sid, latchKeyUp(latchRef.current, e.code));
    });
    const kc = addListener('pointerup', (e) => {
      if (!buttons.current.has(e.button)) return;
      buttons.current.delete(e.button);
      const wait = held.current - Date.now(), b = e.button;
      if (wait > 0) setTimeout(() => lord.vm.button(sid, b, 0), wait + 10); else lord.vm.button(sid, b, 0);
    });
    return () => { ka(); kb(); kc(); lord.vm.release(sid); buttons.current.clear(); dropLatched(); };
  }, [live, focused, sid]);
  // ❖, ⌥Tab and the desktop's other shortcuts go to the machine too, once the person allowed it (asked the first
  // time); ❖Esc gives them back and takes them again (sdk/kbinhibit.js)
  // never, when the machine's profile says so (app.json input.shortcuts 'never'): LordOS keeps them, not even asked
  useEffect(() => (live && focused && readMeta(dir).input?.shortcuts !== 'never' ? inhibitShortcuts(winId, 'machines') : undefined), [live, focused, winId]);

  const at = (e) => (box.current && st?.w ? toPicture(e.x, e.y, lord.getLayout(box.current.id), st.w, st.h) : null);
  const move = (e) => { const q = live && at(e); if (q) lord.vm.motion(sid, q[0], q[1]); };
  // a click that just carried a new clipboard in waits a moment, so the guest has it when the press opens a menu with
  // Paste (a right-click); its release keeps the order
  const held = useRef(0);
  const down = (e, fresh) => {
    if (!live) return;
    if (!capRef.current.on) capDo({ type: 'click', full, autoFs: !!cfgRef.current?.captureFullscreen, mouse: readMeta(dir).input?.mouse });
    move(e);
    buttons.current.add(e.button);
    const b = e.button, id = sid;
    if (fresh) { held.current = Date.now() + CLIP_HOLD_MS; setTimeout(() => lord.vm.button(id, b, 1), CLIP_HOLD_MS); }
    else lord.vm.button(sid, b, 1);
  };
  const onWheel = (e) => {
    if (!live) return;
    move(e);
    const [n, rest] = wheelSteps(wheel.current, e.deltaY || 0, !!e.precise);
    wheel.current = rest;
    if (n) lord.vm.wheel(sid, n, 0);
  };
  const cmd = (c) => { if (sid) { if (!prot && c === 'shutdown' && st?.state === 'running' && (!cfgRef.current?.agent || st?.agent?.state === 'connected')) lord.vm.snapshot(sid); lord.vm.cmd(sid, c); } };
  const sendCombo = (codes) => { if (sid) keysTo(sid, comboSend(codes, latchRef.current)); };
  const cad = () => sendCombo(CTRL_ALT_DEL);
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  // the Info menu's numbers (model.js infoLines): two stats reads INFO_MS apart, while it runs
  const [infoU, setInfoU] = useState(null);
  const infoPrev = useRef(null);
  const running = !!sid && (st?.state === 'running' || st?.state === 'paused');
  // All Displays (5.4, alldisplays.jsx): one output of the guest's display full screen on each of this computer's displays
  const ad = useAllDisplays({ sid, dir, name, winId, st, prot, appsOn, begin });
  useEffect(() => {
    if (!running || !has()) { setInfoU(null); infoPrev.current = null; return undefined; }
    const timer = setInterval(() => {
      const s2 = lord.vm.stats(dir) || {};
      setInfoU(usage(infoPrev.current, s2, cfgRef.current?.vcpus || 1));
      infoPrev.current = s2.at ? s2 : null;
    }, INFO_MS);
    return () => clearInterval(timer);
  }, [running]);
  const [host] = useState(() => (has() ? lord.vm.host() : null));
  const gaps = isolationGaps(host, st);
  // View: Picture in Picture on the window's display, Actual Size (the guest's pixels, while the resolution is fixed)
  const myFrame = () => { const w = windows.get().find((x) => x.id === winId); return w ? wm.frame(w) : null; };
  const toPip = () => { if (protRef.current) return; const f0 = myFrame(); const f = f0 && pipFrame(displayFor(f0), st?.w, st?.h, direction() === 'rtl'); if (f) wm.setPip(winId, f); };
  const fromPip = () => wm.setPip(winId, null);
  const actual = () => { const f0 = myFrame(); const f = f0 && actualFrame(st?.w, st?.h, BAR, workAreaOf(displayFor(f0))); if (f) wm.resize(winId, f.w, f.h); };
  const scaleTo = (k) => { const f0 = myFrame(); const f = f0 && scaleFrame(st?.w, st?.h, k, BAR, workAreaOf(displayFor(f0))); if (f) wm.resize(winId, f.w, f.h); };
  const m0 = { dir, name, id: winId.split('#vm-')[1] };
  const shot = () => {
    const path = sid ? lord.vm.screenshot(sid) : '';
    notify({ app: 'machines', label: tr('machines.title'), title: path ? tr('machines.tb.shotSaved') : tr('machines.tb.shotFailed'), body: path ? path.split('/').pop() : '', sound: false,
      actions: path ? [{ id: 'show', label: tr('machines.cmd.showInFiles'), run: () => revealPath(path) }] : [] });
  };
  // a long request of lord-vm's in progress (suspend, resume, a snapshot): its card, and no second one
  const job = st?.job?.status === 'running' ? st.job : null;
  const busy = !!job;
  // the CD/DVD drive while the machine runs (lord-vm changes the disc through QMP; machine.json follows)
  const cdOk = !!sid && (st?.state === 'running' || st?.state === 'paused') && cdWant === null;
  const cdTo = (path) => { setCdErr(''); if (lord.vm.cdrom(sid, path || null)) setCdWant(path); else setCdErr(tr('machines.tb.cdRefused')); };
  // Take Snapshot from the window: named by time, with the live picture; lord-vm reports it as a VM_JOB
  const snapNow = () => {
    const d = new Date();
    const r = lord.vm.snapTake(dir, tr('machines.snap.atTime', { time: `${d.getHours()}:${String(d.getMinutes()).padStart(2, '0')}` }), sid);
    if (!r) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.snap.failed'), body: '', sound: false });
    else snapJobs.current.push(r.job);
  };
  // Record (6.20): the machine's own picture into ~/Pictures/Screenshots, no screen permission; a second click stops it
  const record = () => {
    if (rec) { media.stop(rec.id); return; }
    const id = sid ? lord.vm.record(sid) : 0;
    if (!id) { notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.tb.recordFailed'), body: '', sound: false }); return; }
    setRec({ id, at: now() });
    const off = media.on(id, (type, d) => {
      if (type !== 'done') return;
      off();
      setRec(null);
      const secs = Math.round((d.duration || 0) / 1000);
      notify({ app: 'machines', label: tr('machines.title'), title: d.ok ? tr('machines.tb.recSaved') : tr('machines.tb.recordFailed'),
        body: d.ok ? tr('machines.tb.recSavedBody', { file: (d.path || '').split('/').pop(), m: Math.floor(secs / 60), s: secs % 60 }) : '', sound: false,
        actions: d.ok && d.path ? [{ id: 'open', label: tr('machines.tb.open'), run: () => openFileWithApps(d.path, (d.path || "").split("/").pop()) }, { id: 'show', label: tr('machines.cmd.showInFiles'), run: () => revealPath(d.path) }] : [] });
    });
  };
  const recSecs = rec ? Math.floor((now() - rec.at) / 1000) : 0;
  // the shared clipboard (6.8), through the agent's channel and only the ways the machine allows: the host's clipboard
  // (text, rich text, a picture) goes in when the window comes to the front, the guest's comes out as it changes; a
  // guest's picture is re-encoded on this side before any app sees it
  const cfgRef = useRef({});
  const [cfg, setCfg] = useState({});
  useEffect(() => { if (!has()) return; const m = lord.vm.list().find((x) => x.dir === dir); if (m) { if (!cfgRef.current?.dir && m.resolution && m.resolution !== 'follow') setFollow(false); cfgRef.current = m; setCfg({ agent: m.agent, clipboard: m.clipboard, clipboardAsk: !!m.clipboardAsk, shares: m.shares || [], transfer: !!m.transfer, windows: !!m.windowsDevices, arch: m.arch, guestLinks: !!m.guestLinks, guestNotify: !!m.guestNotify, network: m.network, group: m.group || '' }); } }, [sid, st?.state]);
  // the devices' own backend (USB, the camera): shown only once it says they're supported here (deviceCaps)
  const caps = has() && typeof lord.vm.deviceCaps === 'function' ? lord.vm.deviceCaps(dir) : null;
  const cableOk = cableOffered(cfg, running);
  const cableOut = !!st?.cableOut;
  // shape keeping (5.3): windowed, the resolution fixed, nothing else deciding the frame
  shapeRef.current = !full && !pip && !appsOn && !follow;
  // "Capture mouse" (capture.js, docs/VIRTUALIZATION.md 6.25): lord-server locks the pointer inside the picture and the
  // guest gets relative motion; ❖Esc or Ctrl+Alt (lord-server's, before any app), focus loss, apps mode or the machine
  // stopping give it back. The state belongs to the window: full screen rebuilds it, and the lock moves to the new picture
  const [cap, setCap] = useWindowState('vm.capture', CAPTURE_IDLE);
  const capRef = useRef(cap);
  const capEnds = useRef(null);
  const [capFull, setCapFull] = useWindowState('vm.captureFull', full);
  const [capNotice, setCapNotice] = useState(false);
  const capOffer = captureOffer({ appsOn: appsOn || appsRef.current, live });
  const capDo = (ev) => {
    const r = captureStep(capRef.current, ev, capOffer.offered && focused);
    let next = r.st;
    if (r.cmd === 'capture') {
      const ok = !!box.current && !!sid && lord.vm.capture(sid, true, box.current.id);
      if (!ok) next = captureStep(next, { type: 'failed' }).st;
      else if (r.notice) setCapNotice(true);
    } else if (r.cmd === 'release' && sid) lord.vm.capture(sid, false);
    capRef.current = next;
    setCap(next);
  };
  // a window rebuilt while captured (full screen): the lock moves to its picture, once that has been laid out (lord-server
  // keeps the lock half a second for it, pointerlock.h)
  useEffect(() => {
    if (!capRef.current.on) return undefined;
    const t2 = setTimeout(() => { if (capRef.current.on && sid && box.current && !lord.vm.capture(sid, true, box.current.id)) capDo({ type: 'failed' }); }, 120);
    return () => clearTimeout(t2);
  }, []);
  // the profile's mouse 'capture' (app.json input.mouse): captured whenever the window comes to the front with its picture
  // up (after that first layout, as for full screen)
  useEffect(() => {
    if (!focused) { capDo({ type: 'blur' }); return undefined; }
    if (!capOffer.offered) { capDo({ type: 'unavailable' }); return undefined; }
    if (readMeta(dir).input?.mouse !== 'capture' || capRef.current.on) return undefined;
    const t2 = setTimeout(() => capDo({ type: 'focus', mouse: readMeta(dir).input?.mouse }), 150);
    return () => clearTimeout(t2);
  }, [focused, capOffer.offered]);
  useEffect(() => {
    if (full === capFull) return undefined;
    setCapFull(full);
    // after the rebuilt window's first layout: the lock needs the picture's box
    const t2 = setTimeout(() => capDo({ type: 'fullscreen', full, autoFs: !!cfgRef.current?.captureFullscreen }), 150);
    return () => clearTimeout(t2);
  }, [full]);
  // lord-server ended it (❖Esc, Ctrl+Alt, the picture gone): seen in the poll
  useEffect(() => {
    if (!st) return;
    if (captureEndedByServer(capRef.current, capEnds.current, st)) capDo({ type: 'ended' });
    capEnds.current = st.captureEnds;
  }, [st?.captureEnds, st?.captured]);
  useEffect(() => { if (!capNotice) return undefined; const t2 = setTimeout(() => setCapNotice(false), CAPTURE_NOTICE_MS); return () => clearTimeout(t2); }, [capNotice]);
  const capOn = !!cap.on && !!st?.captured;
  const clipSent = useRef('');
  // The clipboard's gate: in use while this window, or one of the machine's apps mode windows, is in front (and a few
  // seconds after, held by lord-server). Only then does the guest get the host's clipboard, and only then is its own
  // taken. Its host → guest half: when the person turns to the machine (the window coming to the front, a click on its
  // picture, never a mere pass of the pointer) the guest gets an offer (which forms there are, no contents) and asks
  // for the data when a program there pastes; an older guest helper gets it all at once then. → true when something
  // new went in
  const guestFocused = useStore(guestFocus, (f) => !!f[dir]);
  const inUse = clipInUse(sid, focused, guestFocused);
  const ask = !!cfg.clipboardAsk;
  const gate = (on) => { if (sid && lord.vm.clipboardGate) lord.vm.clipboardGate(sid, on, ask); };
  const inUseRef = useRef(inUse);
  inUseRef.current = inUse;
  useEffect(() => { gate(inUse); }, [sid, inUse, ask]);
  // a click opens the gate for that moment only: on, then back to the window's real state at once, which leaves
  // lord-server's grace (CLIP_GRACE_MS) and nothing longer when the window isn't in front
  const touchGate = () => { gate(true); gate(inUseRef.current); };
  // "Ask before each paste" (machine.json clipboard_ask): the bar's state (model.js clipAsk*) and what each answer does
  const askRef = useRef(askSt);
  askRef.current = askSt;
  const lazy = !!(st?.agent?.caps & AGENT_CAPS.clipLazy);
  // approved: the preview's hash the person said yes to (ask mode; lord-server refuses a push without it there)
  const pushNow = (approved) => (lord.vm.clipboardSendHost ? !!lord.vm.clipboardSendHost(sid, approved) : false);
  // seq (ask mode): the copy the person said yes to; lord-server refuses if the guest's clipboard changed since
  const applyGuest = (seq) => {
    const kind = lord.vm.clipboardApply ? lord.vm.clipboardApply(sid, seq) : 'text';
    if (kind !== 'text') return;
    const text = lord.vm.clipboardTake(sid, seq);
    if (text) { clipSent.current = text; clipboard.set(text, { origin: 'machines' }); }
  };
  const settle = (a) => {
    if (!a || !sid) return;
    if (a.gen) lord.vm.clipboardAnswer(sid, a.gen, a.yes);
    else if (a.key) { if (a.yes) pushNow(a.key); }   // only what was shown: lord-server checks the hash
    else if (a.seq && a.yes) applyGuest(a.seq);
  };
  const askStep = (r) => { askRef.current = r.state; setAskSt(r.state); settle(r.answer); };
  const sendClip = (click) => {
    if (!(inUse || click) || !sid || !clipTo(cfg.clipboard) || st?.agent?.state === undefined || !(st?.agent?.caps & AGENT_CAPS.clipboard)) return false;
    if (click) touchGate();
    // a helper that can't ask for it gets the clipboard pushed: in ask mode only after the person says yes
    if (ask && !lazy && lord.vm.clipboardPreview) {
      const pv = lord.vm.clipboardPreview(sid, 'host');
      if (pv?.fresh) askStep(clipAskPush(askRef.current, pv.hash, now()));
      return false;
    }
    if (lord.vm.clipboardSendHost) return pushNow();
    const text = clipboard.get();
    if (text && text !== clipSent.current && lord.vm.clipboardSend(sid, text)) { clipSent.current = text; return true; }
    return false;
  };
  const sendClipRef = useRef(sendClip);
  sendClipRef.current = sendClip;
  useEffect(() => { if (inUse) sendClip(); }, [inUse, sid, cfg.clipboard, st?.agent?.caps]);
  const clipSeen = useRef(0);
  useEffect(() => {
    if (!sid || !st?.clipIn || st.clipIn === clipSeen.current) return;
    clipSeen.current = st.clipIn;
    if (!clipFrom(cfg.clipboard)) return;
    if (ask) { askStep(clipAskCopied(askRef.current, st.clipIn, now())); return; }
    applyGuest();
  }, [st?.clipIn]);
  // a program in the machine pastes (ask mode): lord-server holds the request until the person answers
  useEffect(() => { if (sid && st?.clipAsk > 0) askStep(clipAskRequest(askRef.current, st.clipAsk, now())); }, [st?.clipAskSeq, st?.clipAsk]);
  useEffect(() => {
    if (!askRef.current.in && !askRef.current.out) return undefined;
    const timer = setInterval(() => askStep(clipAskTick(askRef.current, now())), 1000);
    return () => clearInterval(timer);
  }, [askSt]);
  useEffect(() => { askRef.current = clipAskEmpty(); setAskSt(askRef.current); }, [sid]);
  const askPreview = askKind && sid && lord.vm.clipboardPreview ? lord.vm.clipboardPreview(sid, askKind === 'out' ? 'guest' : 'host') : null;
  const askBar = askKind ? { kind: askKind, name, preview: askPreview } : null;
  const askAnswer = (yes) => askStep(clipAskAnswer(askRef.current, yes));
  // a guest that reached for the clipboard while it wasn't in use (refused or ignored by lord-server): told once per burst
  const blocked = useRef({});
  useEffect(() => {
    if (!sid || !st) return;
    const r = clipBlocked(blocked.current, st.clipBlockedRead, st.clipBlockedWrite, now());
    blocked.current = r.seen;
    for (const k of r.notify) notify({ app: 'machines', label: tr('machines.title'), title: tr(k === 'read' ? 'machines.clip.blockedRead' : 'machines.clip.blockedWrite', { name }), body: tr('machines.clip.blockedBody'), sound: false });
  }, [st?.clipBlockedRead, st?.clipBlockedWrite]);
  useEffect(() => { blocked.current = {}; }, [sid]);
  // sound and microphone (6.9): what this window's session reports (Configure's changes reach it at once)
  const snd = st?.sound ? { sound: st.sound.on, volume: st.sound.volume, microphone: st.sound.microphone } : soundOf(cfgRef.current);
  const devs = soundDevs(st?.sound || cfgRef.current);
  const sndSt = useSound();
  const heard = audible(snd);
  const soundTo = (patch) => { if (has() && !lord.vm.sound(dir, patch)) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.cfg.soundFailed'), body: '', sound: false }); };
  const toggleSound = () => soundTo(muteToggle(snd));
  // the guest records and the machine allows the microphone: LordOS's own permission prompt, once per window (C checks
  // it again before it opens the microphone)
  const micAsked = useRef(false);
  useEffect(() => { if (micAsk(st?.sound, micAsked.current)) { micAsked.current = true; requestPermission('machines', 'microphone'); } }, [st?.sound?.mic]);
  const micOn = micLive(st?.sound);
  // the capture indicator names the machine: "Machines — Windows 11 Pro"
  const micWho = name || '';
  useEffect(() => { markDeviceUse('machines', 'microphone', micOn, null, micWho); return micOn ? () => markDeviceUse('machines', 'microphone', false, null, micWho) : undefined; }, [micOn, micWho]);
  // Files dragged from Files onto the picture (6.x): copied into the guest's Downloads by its agent, one at a time in
  // lord-vm; a line under the picture while they go, a notice when they're there. Only into the guest: nothing comes
  // out by dragging (a shared folder does that)
  const blocker = dropBlocker(st, !!cfg.agent);
  // the transfer folder (6.7.1): Open Transfer Folder, and for a Windows guest, WinFsp (the admin helper downloads the
  // catalog's pinned installer once; lord-server hands lord-vm only that file from the image store)
  const xstate = transferState(cfg, st);
  const xferOpen = () => { const p = has() ? lord.vm.transferFolder(dir) : null; if (p) openFolder(p); };
  const xferBusy = useRef(false);
  const xferSetup = async () => {
    const s = sidRef.current;
    const f = winfspFor(CATALOG, cfg.arch || 'amd64');
    if (!s || !f || xferBusy.current) return;
    xferBusy.current = true;
    try {
      const path = String(await admin.run('vm-image-get', `winfsp ${cfg.arch || 'amd64'} ${f.name} ${f.sha}`, null, { quiet: true })).trim();
      if (!lord.vm.agent(s, 'transfer', path)) throw new Error(tr('machines.tb.transferFailed', { machine: name }));
    } catch (e) {
      notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.tb.transferFailed', { machine: name }), body: e?.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e?.message || '', sound: false });
    } finally { xferBusy.current = false; }
  };
  // how it went (poll's agent.transfer: 2 done, 3 failed), said once
  const xferSaid = useRef(0);
  useEffect(() => {
    const r = st?.agent?.transfer;
    if (r !== 2 && r !== 3) { xferSaid.current = 0; return; }
    if (xferSaid.current === r) return;
    xferSaid.current = r;
    notify({ app: 'machines', label: tr('machines.title'), title: tr(r === 2 ? 'machines.tb.transferReady' : 'machines.tb.transferFailed', { machine: name }),
      body: r === 2 ? tr('machines.win.transferDrive') : st.agent.transferMessage || '', sound: false });
  }, [st?.agent?.transfer]);
  // after an unattended Windows install the app sets it up by itself, once (app.json transferSetup)
  useEffect(() => {
    if (!autoTransferSetup(readMeta(dir), xstate)) return;
    writeMeta(dir, { transferSetup: false });
    xferSetup();
  }, [xstate]);
  const sendFiles = (files) => {
    const s = sidRef.current;
    const why = dropBlocker(stRef.current, !!cfg.agent);
    if (why || !s) { notify({ app: 'machines', label: tr('machines.title'), title: tr(`machines.drop.no.${why || 'off'}`), body: '', sound: false }); return; }
    const refused = files.filter((f) => !lord.vm.sendFile(s, f)).map((f) => f.split('/').pop());
    if (refused.length) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.drop.refused', { name: refused.join(', ') }), body: '', sound: false });
  };
  const drop = useFilesDrop({ onDrop: ({ files }) => sendFiles(files) });
  onFiles.current = (list) => {
    if (!Array.isArray(list)) return;
    setCopies(list.filter((f) => f.state === 'copying'));
    // files opened from Files' "Open with": the program's window comes up as its own (apps mode), or this one in front
    const opened = list.filter((f) => f.open && f.state === 'done'), notOpened = list.filter((f) => f.open && f.state === 'failed');
    for (const f of [...opened, ...notOpened]) opening.current.delete(f.id);
    if (opened.length) {
      if (!appsBlockedRef.current && !appsRef.current) setApps(true);
      else if (!appsRef.current) wm.focus(winId);
    }
    for (const f of notOpened) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.openWith.failed', { machine: name, name: f.name }), body: f.message, sound: false });
    // files dropped on an apps mode window (appsmode.jsx): its copy line, and how the guest took them
    if (appsRef.current) guests.set((x) => (x[dir] ? { ...x, [dir]: { ...x[dir], copies: list.filter((f) => f.place && f.state === 'copying') } } : x));
    const placed = list.filter((f) => f.place && f.state === 'done');
    if (placed.some((f) => f.placed === 'clipboard')) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.apps.dropPaste', { machine: name }), body: '', sound: false });
    if (placed.some((f) => f.placed === 'folder')) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.apps.dropFolder', { machine: name }), body: '', sound: false });
    if (placed.some((f) => f.placed === 'window')) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.apps.dropWindow', { machine: name }), body: '', sound: false });
    list = list.filter((f) => !f.open && !(f.place && f.state !== 'failed'));
    const done = list.filter((f) => f.state === 'done'), failed = list.filter((f) => f.state === 'failed');
    if (done.length) {
      notify({ app: 'machines', label: tr('machines.title'), title: done.length === 1 ? tr('machines.drop.done', { machine: name }) : tr('machines.drop.doneN', { machine: name, count: done.length }),
        body: done.length === 1 ? done[0].message : guestFolder(done[done.length - 1].message), sound: false });
    }
    for (const f of failed) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.drop.failed', { machine: name, name: f.name }), body: f.message, sound: false });
  };
  const line = copyLine(copies);
  // Files' "Open with" this machine (7.3; openwith.js): the files asked for wait here until the machine runs and its
  // guest tools can open them, then go in as drops with { open }; once one is open, apps mode (when the guest has it)
  const asks = useStore(openAsks, (x) => x[dir] || null);
  const opening = useRef(new Map());          // request id → the file's name
  const step = asks ? openStep(sid, st) : null;
  useEffect(() => {
    if (!asks) return;
    if (step === 'resume') { lord.vm.cmd(sid, 'resume'); return; }
    // 'start': openInMachine opened this window (a new one starts its machine) or asked it to (playAsks)
    if (step === 'cant') {
      openAsks.set((x) => withoutAsks(x, dir));
      for (const a of asks) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.openWith.failed', { machine: name, name: a.path.split('/').pop() }), body: tr('machines.openWith.cant'), sound: false });
      return;
    }
    if (step !== 'send') return;
    openAsks.set((x) => withoutAsks(x, dir));
    for (const a of asks) {
      const id = lord.vm.sendFile(sid, a.path, { open: true });
      if (id) opening.current.set(id, a.path.split('/').pop());
      else notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.openWith.failed', { machine: name, name: a.path.split('/').pop() }), body: tr('machines.openWith.refused'), sound: false });
    }
  }, [asks, step, sid]);
  // the tools never came (no one signed in, or they aren't installed after all): said, and forgotten
  useEffect(() => {
    if (!asks) return undefined;
    const t2 = setInterval(() => {
      const now = Date.now();
      if (!asks.some((a) => askStale(a, now))) return;
      openAsks.set((x) => withoutAsks(x, dir));
      for (const a of asks) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.openWith.failed', { machine: name, name: a.path.split('/').pop() }), body: tr('machines.openWith.noTools'), sound: false });
    }, 5000);
    return () => clearInterval(t2);
  }, [asks]);
  // the machine's tools said whether they open files: kept, so Files lists it while it's off too
  const opensNow = opensOf(st?.agent);
  useEffect(() => { if (opensNow !== undefined && (readMeta(dir).opens ?? false) !== opensNow) writeMeta(dir, { opens: opensNow }); }, [opensNow]);
  // The LordOS tools brought up to date by themselves (model.js toolsAutoStep): the same install as the popover's
  // button, only while nothing is on its way; off or given up, a notification with Update. Its state lives for the
  // machine's run (toolsRuns), so the window closed and opened again doesn't try again.
  const toolsBusy = useRef(false);
  const gl = guests.get()[dir];
  toolsBusy.current = copies.length > 0 || opening.current.size > 0 || !!asks || xferBusy.current || xstate === 'settingUp' || st?.agent?.transfer === 1
    || Object.values(gl?.launches || {}).some((e) => launchPending(e, gl.list, Date.now()));
  useEffect(() => {
    if (!sid) return undefined;
    const say = (why, agent) => {
      const have = agent?.tools || '', want = toolsWant(agent);
      notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.agent.toolsOldTitle', { machine: name || dir.split('/').pop() }),
        body: tr(why === 'off' ? 'machines.agent.toolsOldBody' : 'machines.agent.toolsAutoFailed', { have, want }), sound: false,
        defaultAction: { id: 'show', run: () => wm.focus(winId) },
        actions: [{ id: 'update', label: tr('machines.agent.toolsUpdateBtn'), run: () => { const x = sidRef.current; if (x) lord.vm.agent(x, 'install'); } }] });
    };
    const timer = setInterval(() => {
      const q = stRef.current;
      if (!q || q.ended || q.state !== 'running' || closingRef.current) return;
      const t0 = Date.now();
      const r = toolsAutoStep(toolsRuns.get(dir), { agent: q.agent, on: toolsAutoOn(readMeta(dir), defaultsOf(settings.get().machinesDefaults)), busy: toolsBusy.current, now: t0 });
      let s = r.s, why = r.act === 'notify' ? r.why : null;
      // lord-server didn't take it (the session is going away): a failed attempt
      if (r.act === 'install' && !lord.vm.agent(sid, 'update')) {
        s = toolsAutoFailed(s, t0);
        if (s.tries >= TOOLS_TRIES && s.told !== s.want) { s = { ...s, told: s.want }; why = 'failed'; }
      }
      toolsRuns.set(dir, s);
      if (why) say(why, q.agent);
    }, 1000);
    return () => clearInterval(timer);
  }, [sid, name]);
  // apps mode (7.2): the guest's windows as LordOS windows (appsmode.jsx); this window becomes a small controller
  const appsBlocked = appsBlocker(st?.agent, cfg);
  appsBlockedRef.current = appsBlocked;
  const setApps = (on) => {
    setAppsOn(on);
    writeMeta(dir, { appsMode: on });
    if (!on) sized.current.last = null;            // the guest takes this window's size again (Follow)
  };
  const appsActive = useAppsMode({ dir, sid, st, name, mid: m0.id || dir.split('/').pop(), home: winId, on: appsOn, ready: !appsBlocked, onLeave: () => setApps(false), onTouch: () => sendClipRef.current(true) });
  // apps mode: the guest window in front shows the bar instead
  useEffect(() => { if (appsActive) guests.set((g) => (g[dir] ? { ...g, [dir]: { ...g[dir], ask: askBar, askAnswer } } : g)); }, [appsActive, askSt]);
  appsRef.current = appsActive;
  useEffect(() => {
    const w = wm.get(winId);
    if (appsActive) {
      if (w && !preApps.current) preApps.current = { w: w.w, h: w.h };
      if (full) wm.setFullscreen(winId, false);
      // the controller's panel: the machine's bar, Running and the guest's apps (AppsController); taller with the
      // console open under it
      wm.resize(winId, 420, shell ? 860 : 600);
    } else if (preApps.current) { wm.resize(winId, preApps.current.w, preApps.current.h); preApps.current = null; }
  }, [appsActive, appsActive && shell]);
  const gApps = useStore(guests, (x) => x[dir] || null);
  const gRunning = runningKey(runningWindows(gApps?.list));
  // why it can't be on: the agent, or a guest whose helper can't list its windows (it needs LordOS or Windows inside)
  const appsWhy = appsBlocked === 'needsTools' ? tr('machines.win.appsNeedsTools') : appsBlocked ? tr('machines.apps.needsAgent') : '';
  const appsLabel = appsOn ? tr('machines.apps.leave') : appsWhy || tr('machines.apps.enter');
  // the Keys panel: a Windows guest gets the Windows key and its combinations, any other the Super key and Linux's
  const keys = {
    windows: !!cfg.windows || /^windows/i.test(st?.agent?.system || '') || familyOf({ ...cfgRef.current, meta: readMeta(dir), name }) === 'windows',
    latched, ok: !!sid && a.keys, hold: !!sid && a.keys && live && focused,
    toggle: (c) => { if (sid && live && wm.hasKeyboard(winId)) keysTo(sid, latchToggle(latchRef.current, c)); },
    send: sendCombo,
  };
  const ctl = {
    keys,
    appsOn, appsBlocked, appsLabel, toggleApps: () => setApps(!appsOn),
    sid, cfg, heard, toggleSound, micLive: micOn,
    // the speaker's menu: mute, the output and the input (each applied at once, schema 8), the sound settings
    soundMenu: [
      { label: heard ? tr('machines.tb.mute') : tr('machines.tb.unmute'), onClick: toggleSound },
      '-',
      { label: tr('machines.cfg.soundOutput'), note: true },
      ...soundDevItems(sndSt.sinks, devs.output, (x) => soundTo({ output: x })),
      '-',
      { label: tr('machines.cfg.soundInput'), note: true },
      ...soundDevItems(microphones(sndSt), devs.input, (x) => soundTo({ input: x })),
      '-',
      { label: tr('machines.tb.soundSettings'), onClick: () => setCfgSheet({ section: 'sound' }) },
    ],
    xfer: { state: xstate, open: xferOpen, setup: xferSetup, windows: !!cfg.windows || /^windows/i.test(st?.agent?.system || '') },
    shell, toggleShell: () => { setShell(!shell); setShellKeys(!shell); },
    rec: !!rec, recTime: `${String(Math.floor(recSecs / 60)).padStart(2, '0')}:${String(recSecs % 60).padStart(2, '0')}`, record,
    st, a, full, follow, cmd,
    capOn, capOffer, toggleCapture: () => capDo({ type: 'toggle' }),
    setFull: (on) => wm.setFullscreen(winId, on),
    setFollow: (on) => { setFollow(on); sized.current.last = null; },
    fixed: (w, h) => { setFollow(false); if (sid) lord.vm.resize(sid, w, h); },
    stuck,
    logs: () => openLogs(m0),
    configure: (section) => setCfgSheet({ section }),
    begin: () => begin(),
    screenshot: shot,
    prot,     // a protected machine: no screenshot or recording of its picture (lord.vm.protect refuses them too)
    power: [
      { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => cmd('shutdown') },
      { label: tr('machines.cmd.suspend'), disabled: !a.suspend || busy || noSuspend(st, gpu), onClick: () => cmd('suspend') },
      { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => cmd('reset') },
      { label: tr('machines.cmd.cad'), disabled: !a.keys, onClick: cad },
      '-',
      { label: tr('machines.cmd.poweroff'), destructive: true, disabled: !a.poweroff, onClick: () => cmd('poweroff') },
    ],
    devices: [
      { label: iso ? tr('machines.tb.cdIn', { name: iso.split('/').pop() }) : tr('machines.tb.cdNone'), disabled: true },
      { label: tr('machines.tb.cdChoose'), disabled: !cdOk, onClick: () => setPickCd(true) },
      { label: tr('machines.tb.cdEject'), disabled: !cdOk || !iso, onClick: () => cdTo('') },
      '-',
      { label: tr('machines.tb.sound'), checked: heard, onClick: toggleSound },
      { label: tr('machines.cfg.microphone'), checked: snd.microphone, onClick: () => soundTo({ microphone: !snd.microphone }) },
      { label: tr('machines.tb.soundSettings'), onClick: () => setCfgSheet({ section: 'sound' }) },
      '-',
      // the network (5.3): its mode, the cable while it runs (VM_NETCABLE), its settings
      { label: tr('machines.dev.network', { mode: tr(`machines.cfg.net.${cfg.network || 'shared'}`) }), disabled: true },
      { label: tr('machines.dev.cable'), checked: cableOk && !cableOut, disabled: !cableOk, hint: !cableOk && running ? tr('machines.dev.cableNo') : '',
        onClick: () => { if (sid) lord.vm.netCable(sid, cableOut); } },
      { label: tr('machines.dev.networkSettings'), onClick: () => setCfgSheet({ section: 'network' }) },
      '-',
      { label: (cfg.shares || []).length ? tr('machines.tb.foldersN', { n: cfg.shares.length }) : tr('machines.tb.folders'), disabled: true },
      { label: tr('machines.dev.folderSettings'), onClick: () => setCfgSheet({ section: 'sharing' }) },
      // USB and the camera: their own backend's, only once it says they're supported (deviceCaps)
      ...(caps?.usb ? ['-', { label: tr('machines.dev.usb'), onClick: () => setCfgSheet({ section: 'usb' }) }] : []),
      ...(caps?.camera ? [{ label: tr('machines.dev.camera'), onClick: () => setCfgSheet({ section: 'camera' }) }] : []),
    ],
    dots: deviceDots({ iso, heard, mic: micOn, cableOut, network: cfg.network, shares: (cfg.shares || []).length, transfer: cfg.transfer, caps }),
  };
  useEffect(() => {
    wm.setMenus(winId, [
      { label: tr('machines.menu.actions'), items: [
        { label: a.resume ? tr('machines.cmd.resume') : tr('machines.cmd.pause'), disabled: !a.pause && !a.resume, onClick: () => cmd(a.resume ? 'resume' : 'pause') },
        { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => cmd('reset') },
        { label: tr('machines.cmd.cad'), disabled: !a.keys, onClick: cad },
        { label: tr('machines.keys.send'), disabled: !a.keys, submenu: sendKeysItems(keys) },
        '-',
        { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => cmd('shutdown') },
        { label: tr('machines.cmd.suspend'), disabled: !a.suspend || busy || noSuspend(st, gpu), onClick: () => cmd('suspend') },
        { label: tr('machines.cmd.poweroff'), disabled: !a.poweroff, onClick: () => cmd('poweroff') },
        '-',
        { label: tr('machines.snap.takeNow'), disabled: !sid || busy || noSuspend(st, gpu) || !(st?.state === 'running' || st?.state === 'paused'), onClick: snapNow },
        { label: tr('machines.cmd.snapshots'), onClick: () => openSnapshots(m0) },
        '-',
        { label: tr('machines.tb.transferOpen'), disabled: !cfg.transfer, onClick: xferOpen },
        ...(xstate === 'setup' ? [{ label: tr('machines.tb.transferSetup'), onClick: xferSetup }] : []),
        '-',
        { label: tr('machines.tb.screenshot'), disabled: !a.keys || prot, onClick: shot },
        { label: full ? tr('machines.cmd.exitFullscreen') : tr('machines.cmd.fullscreen'), onClick: () => wm.setFullscreen(winId, !full) },
        { label: tr('machines.tb.capture'), checked: capOn, disabled: !capOffer.offered, hint: capOffer.why === 'apps' ? tr('machines.tb.captureApps') : '', onClick: () => capDo({ type: 'toggle' }) },
        { label: tr('machines.cmd.logs'), onClick: () => openLogs(m0) },
        { label: tr('machines.cmd.configure'), onClick: () => setCfgSheet({}) },
        '-',
        { label: tr('machines.apps.mode'), checked: appsOn, disabled: !!appsBlocked && !appsOn, hint: !appsOn ? appsWhy : '', onClick: () => setApps(!appsOn) },
      ] },
      { label: tr('machines.menu.devices'), items: ctl.devices.filter((x) => x !== '-') },
      { label: tr('machines.lib.view'), items: [
        { label: tr('machines.view.window'), checked: !full && !pip && !appsOn, onClick: () => { if (appsOn) setApps(false); if (full) wm.setFullscreen(winId, false); if (pip) fromPip(); } },
        { label: tr('machines.cmd.fullscreen'), checked: full, onClick: () => wm.setFullscreen(winId, !full) },
        { label: tr('machines.apps.mode'), checked: appsOn, disabled: !!appsBlocked && !appsOn, hint: !appsOn ? appsWhy : '', onClick: () => setApps(!appsOn) },
        { label: tr('machines.view.pip'), checked: pip, disabled: !running || appsOn || prot, hint: prot ? tr('machines.view.pipProtected') : '', onClick: () => (pip ? fromPip() : toPip()) },
        ad.item,
        '-',
        { label: tr('machines.view.actual'), disabled: !running || follow || full || pip || !st?.w, hint: follow ? tr('machines.view.actualFollow') : '', onClick: actual },
        { label: tr('machines.view.scale'), disabled: !running || follow || full || pip || !st?.w, hint: follow ? tr('machines.view.actualFollow') : '',
          submenu: SCALES.map((k) => ({ label: tr('machines.view.scaleN', { n: Math.round(k * 100) }), onClick: () => scaleTo(k) })) },
      ] },
      { label: tr('machines.menu.info'), items: [
        ...infoLines(cfgRef.current, infoU, gaps).map((l) => ({ label: tr(l.key, l.vars), disabled: true })),
        ...(gaps.length ? gaps.map((g) => ({ label: tr(`machines.weak.${g}`), disabled: true })) : []),
        '-',
        { label: tr('machines.info.configure'), onClick: () => setCfgSheet({}) },
      ] },
      ...(appsActive ? [{ label: tr('machines.apps.menu'), items: appsMenuItems(gApps) }] : []),
    ]);
  }, [st?.state, full, pip, follow, st?.w, st?.h, infoU, gaps.join(), iso, sid, cdWant, heard, snd.microphone, cableOut, cableOk, cfg.network, (cfg.shares || []).length, caps?.usb, caps?.camera, appsOn, appsBlocked, appsActive, gApps?.apps, gRunning, cfg.transfer, xstate, capOn, capOffer.offered, focused, keys.windows, ad.on, ad.item.disabled, ad.item.hint]);
  // Esc in the controller leaves apps mode (in a guest's window, Esc is the guest's); in its search field, Esc is the
  // field's (it clears the search first)
  useEffect(() => (appsActive && focused && !askClose ? addListener('keydown', (e) => { if (e.key === 'Escape' && !menus.open && !hasTextFocus()) setApps(false); }) : undefined), [appsActive, focused, askClose]);
  // Esc on the close question is Cancel (in apps mode too, where it would otherwise leave apps mode behind the card)
  useEffect(() => (askClose && focused ? addListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault?.(); setAskClose(false); } }) : undefined), [askClose, focused]);

  let card = null;
  let failed = null;
  if (err) card = <Card title={tr('machines.win.problem')} text={err} actions={[<Btn key="r" primary label={tr('machines.win.tryAgain')} onClick={() => { setErr(''); begin(); }} />]} />;
  else if (over) card = <Card width={560} title={tr('machines.budget.title', { name: over.m.name })}
    text={over.waiting ? tr('machines.budget.waiting', { name: over.waiting }) : tr('machines.budget.text', { need: over.b.needMb, free: over.b.freeMb })}
    actions={over.waiting ? [<Btn key="c" label={tr('machines.cancel')} onClick={() => setOver(null)} />] : [
      <Btn key="c" label={tr('machines.cancel')} onClick={() => setOver(null)} />,
      ...over.plan.suspend.map((x) => <Btn key={x.dir} label={tr('machines.budget.suspend', { name: x.name })} onClick={() => suspendFor(x)} />),
      over.plan.lessMb ? <Btn key="l" primary label={tr('machines.budget.less', { mb: over.plan.lessMb })} onClick={() => startWith(over.plan.lessMb)} /> : null,
    ].filter(Boolean)} />;
  else if (askClose) card = <Card width={560} title={tr('machines.close.title', { name })} text={tr('machines.close.text') + (gpu ? ` ${tr('machines.close.noGpuSuspend')}` : noSuspend(st) ? ` ${tr('machines.close.no3dSuspend')}` : '')}
    extra={<View style={{ gap: 8, marginTop: 4 }}>
      {/* a copy into the machine still going: said here, where the person decides (the copy line itself is hidden under a card) */}
      {line ? <Text style={{ fontSize: 13, color: TONE.warn || p.text, textAlign: 'start' }}>{tr(line.fast ? 'machines.drop.copyingFast' : 'machines.drop.copying', { name: line.name, pct: line.pct })}</Text> : null}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}><Toggle on={always} onChange={setAlways} /><Text style={{ fontSize: 13, color: p.text }}>{tr('machines.close.always')}</Text></View>
    </View>} actions={[
    <Btn key="c" label={tr('machines.cancel')} onClick={() => setAskClose(false)} />,
    <Btn key="k" label={tr('machines.close.keep')} onClick={() => { if (always) writeMeta(dir, { onClose: 'keep' }); finish('keep'); }} />,
    <Btn key="s" label={tr('machines.cmd.shutdown')} onClick={() => { if (always) writeMeta(dir, { onClose: 'shutdown' }); finish('shutdown'); }} />,
    <Btn key="u" primary label={tr('machines.cmd.suspend')} disabled={st?.state === 'starting' || noSuspend(st, gpu)} onClick={() => { if (always) writeMeta(dir, { onClose: 'suspend' }); finish('suspend'); }} />,
  ]} />;
  else if (job) card = <Card title={tr(`machines.job.${job.kind}`, { name })} text={job.pct >= 0 ? tr('machines.job.pct', { pct: job.pct }) : tr('machines.job.wait')} />;
  else if (closing === 'suspend') card = <Card title={tr('machines.job.suspend', { name })} text={tr('machines.job.wait')} />;
  else if (closing) card = <Card title={tr('machines.win.shuttingDown', { name })} text={tr('machines.win.shuttingDownHint')} actions={[<Btn key="f" danger label={tr('machines.cmd.poweroff')} onClick={() => cmd('poweroff')} />]} />;
  else if (startPhase(sid, st, starting) === 'off') {
    if (v.why) {
      const flog = log || [st.detail, st.lastError, st.stderr].filter(Boolean).join('\n');
      const m0 = has() ? lord.vm.list().find((x) => x.dir === dir) : null;
      let snaps = 0;
      try { snaps = has() ? (lord.vm.snaps(dir)?.list || []).length : 0; } catch (e) { snaps = 0; }
      const fixes = failActions(failCause(v.why, flog), { memoryMb: m0?.memoryMb || 0, snaps });
      failed = <Failed name={name} v={v} st={st} log={flog} fixes={fixes} onStart={() => begin()} onSettings={() => setCfgSheet({})}
        onUse2d={use2d} busy={!!to2d} onAnyway={() => begin({ resumeAnyway: true })} onDiscard={() => begin({ discardState: true })}
        onLess={startWith} onSnapshot={() => m0 && openSnapshots(m0)} />;
    }
    else if (v.key === 'suspended') card = <OffView picture dir={dir} name={name} title={tr('machines.state.suspended')} hint={tr('machines.win.suspendedHint')} label={tr('machines.cmd.resume')} onPlay={() => begin()} />;
    else card = <OffView dir={dir} name={name} title={tr(`machines.state.${v.key}`)} hint={tr(offHint(st?.reason))} label={tr('machines.start')} onPlay={() => begin()} />;
  } else if (startPhase(sid, st, starting) === 'starting' || st.state === 'starting' || (st.display !== 'up' && st.display !== 'none' && st.display !== 'lost' && (st.state === 'running' || st.state === 'paused'))) {
    card = <Card title={tr('machines.win.starting', { name })} text={tr('machines.win.startingHint')} />;
  } else if (st.display === 'lost') {
    card = <Card title={tr('machines.win.lost')} text={tr('machines.win.lostHint')} actions={[<Btn key="d" primary label={tr('machines.win.reconnect')} onClick={() => lord.vm.cmd(sid, 'display')} />]} />;
  } else if (st.display === 'none') card = <Card title={tr('machines.win.noDisplay')} text={tr('machines.win.noDisplayHint')} />;
  else if (st.state === 'stopping') card = <Card title={tr('machines.state.stopping')} text={tr('machines.win.shuttingDownHint')} />;

  return (
    <View ref={root} {...drop.props} style={{ flex: 1, backgroundColor: '#000' }}>
      {appsActive ? <AppsController name={name} g={gApps} onLeave={() => setApps(false)} ctl={ctl} />
        : <View ref={box} onMouseDown={(e) => { setShellKeys(false); down(e, sendClipRef.current(true)); }} onDoubleClick={pip ? fromPip : undefined} onMouseMove={move} onWheel={onWheel} style={{ flex: 1, marginTop: full || pip ? 0 : BAR }}>
          {st?.media && st.display === 'up' ? <MediaView media={st.media} style={{ flex: 1 }} /> : null}
        </View>}
      {shell && !full ? (
        <View onMouseDown={() => setShellKeys(true)} style={{ height: 260, borderTopWidth: 2, borderColor: shellKeys ? '#0a84ff' : p.line, backgroundColor: '#101114' }}>
          <View style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, backgroundColor: '#1b1c20' }}>
            <View style={{ paddingHorizontal: 6, height: 18, borderRadius: 4, justifyContent: 'center', backgroundColor: '#3a2a5c' }}>
              <Text style={{ fontSize: 11, fontWeight: 700, color: '#d9c9ff' }}>{tr('machines.shell.badge')}</Text>
            </View>
            <Text style={{ flex: 1, fontSize: 12, color: '#c9c9d1', textAlign: 'start' }}>{tr(shellUse === 'agent' ? 'machines.shell.agentTitle' : 'machines.shell.title', { name })}</Text>
            {agentShell ? <Segmented small value={shellUse} onChange={(x) => setShellSrc(x)} options={[['agent', tr('machines.shell.agent')], ['serial', tr('machines.shell.serial')]]} /> : null}
            {shellUse === 'agent' && st?.shell ? <Btn label={tr('machines.shell.new')} onClick={() => lord.vm.shell(sid, 100, 14)} style={{ height: 20, paddingHorizontal: 8 }} /> : null}
            <Text style={{ fontSize: 11, color: '#8a8a94' }}>{shellKeys ? tr('machines.shell.keysHere') : tr('machines.shell.clickHere')}</Text>
            <Btn label="×" onClick={() => { setShell(false); setShellKeys(false); }} style={{ height: 20, paddingHorizontal: 8 }} />
          </View>
          {shellTerm ? <Terminal ref={shellRef} session={shellTerm} style={{ flex: 1 }} />
            : <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 12, color: '#8a8a94' }}>{a.reset ? tr('machines.shell.connecting') : tr('machines.shell.off')}</Text></View>}
        </View>
      ) : null}
      {linkSeq && st?.guestLink?.url ? (
        <View style={{ position: 'absolute', top: BAR + 12, left: 0, right: 0, alignItems: 'center' }}>
          <View style={{ maxWidth: 560, marginHorizontal: 16, padding: 12, gap: 8, borderRadius: 12, backgroundColor: p.dark ? '#2c2c33' : '#fbfbfd', border: `1px solid ${p.line}`, boxShadow: '0 12px 32px rgba(0,0,0,0.4)' }}>
            <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.link.ask', { name: name || '' })}</Text>
            <Text style={{ fontSize: 12, fontFamily: 'monospace', color: p.text, direction: 'ltr', textAlign: 'left', maxHeight: 54, overflow: 'hidden', wordBreak: 'break-all' }}>{st.guestLink.url}</Text>
            <Text style={{ fontSize: 11, color: p.dim, textAlign: 'start' }}>{tr('machines.link.hint')}</Text>
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
              <Btn label={tr('machines.link.ignore')} onClick={() => linkAnswer(false)} />
              <Btn primary label={tr('machines.link.open')} disabled={linkReady !== linkSeq} onClick={() => linkAnswer(true)} />
            </View>
          </View>
        </View>
      ) : null}
      {st?.state === 'paused' && !card ? (
        <View style={{ position: 'absolute', top: BAR + 12, left: 0, right: 0, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 12, height: 26, borderRadius: 13, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.6)' }}>
            <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>{tr(st.reason === 'session-full' ? 'machines.group.sessionFull' : 'machines.state.paused')}</Text>
          </View>
        </View>
      ) : null}
      {card}
      {failed}
      {!card && !failed && !appsActive ? <ClipAskBar ask={askBar} onAnswer={askAnswer} top={(full ? 0 : BAR) + 12} /> : null}
      {!card && !failed && gwBar?.bar ? (
        <View style={{ position: 'absolute', left: 0, right: 0, top: (full ? 0 : BAR) + 12, alignItems: 'center' }}>
          <View style={{ maxWidth: 560, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, paddingVertical: 8, borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.94)', border: '1px solid rgba(255,154,154,0.5)' }}>
            <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: gwBar.bar === 'starting' ? '#ffd60a' : '#ff9a9a' }} />
            <Text style={{ flexShrink: 1, fontSize: 13, lineHeight: 1.4, color: '#f2f2f5', textAlign: 'start' }}>{tr(gwBar.bar === 'starting' ? 'machines.group.gatewayStarting' : 'machines.group.noGateway')}</Text>
            {gwBar.bar === 'off' ? <Btn label={tr('machines.group.startGateway')} onClick={startGateway} style={{ height: 28 }} /> : null}
          </View>
        </View>
      ) : null}
      {!card && !failed && ((noticeOn && noticeText) || cdErr) ? (
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 16, alignItems: 'center' }}>
          <View style={{ maxWidth: 560, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, paddingVertical: 9, borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.94)' }}>
            <Text style={{ flex: 1, fontSize: 13, lineHeight: 1.4, color: '#f2f2f5', textAlign: 'start' }}>{cdErr ? tr('machines.tb.cdFailed', { why: cdErr }) : noticeText}</Text>
            {cdErr ? <Btn label={tr('machines.ok')} onClick={() => setCdErr('')} style={{ height: 28 }} />
              : ['network-back', 'identity'].includes(st.notice) ? <Btn label={tr('machines.ok')} onClick={() => setNoticeGone(st.noticeSeq)} style={{ height: 28 }} />
              : <Btn label={tr('machines.cmd.logs')} onClick={() => openLogs(m0)} style={{ height: 28 }} />}
          </View>
        </View>
      ) : null}
      {asks && !line && !card && !failed ? (
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 64, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ maxWidth: 560, minWidth: 260, paddingHorizontal: 14, paddingVertical: 8, borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.94)' }}>
            <Text style={{ fontSize: 13, lineHeight: 1.4, color: '#f2f2f5', textAlign: 'start' }}>
              {tr(step === 'wait' && st?.state === 'running' ? 'machines.openWith.waitTools' : 'machines.openWith.waitStart', { name: midName(asks[0].path.split('/').pop()), count: asks.length })}
            </Text>
          </View>
        </View>
      ) : null}
      {line && !card && !failed ? (
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 64, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ maxWidth: 560, minWidth: 260, paddingHorizontal: 14, paddingVertical: 8, borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.94)' }}>
            <Text style={{ fontSize: 13, lineHeight: 1.4, color: '#f2f2f5', textAlign: 'start' }}>
              {tr(line.fast ? 'machines.drop.copyingFast' : 'machines.drop.copying', { name: line.name, pct: line.pct }) + (line.waiting ? ` · ${tr('machines.drop.waiting', { count: line.waiting })}` : '')}
            </Text>
            {/* the copy's progress as a bar too, so it reads at a glance whatever the language's line says */}
            <View style={{ marginTop: 6, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.16)', direction: 'ltr' }}>
              <View style={{ width: `${Math.max(2, line.pct)}%`, height: 4, borderRadius: 2, backgroundColor: TONE.run }} />
            </View>
          </View>
        </View>
      ) : null}
      <FilesDropHint over={drop.over} title={drop.over && (blocker ? tr(`machines.drop.no.${blocker}`) : drop.over.files.length ? tr('machines.drop.title') : tr('machines.drop.none'))}
        note={drop.over && !blocker && drop.over.files.length ? `${drop.over.skipped ? `${tr('machines.drop.skipped', { count: drop.over.skipped })} · ` : ''}${tr(`machines.drop.way.${dropWay(cfg, st)}`)}` : ''} />
      {pickCd ? <FilePicker title={tr('machines.new.pickIso')} filter={/\.iso$/i} onClose={() => setPickCd(false)}
        onPick={(x) => { setPickCd(false); cdTo(Array.isArray(x) ? x[0] : x); }} /> : null}
      {capNotice && capOn && !card && !failed ? (
        <View style={{ position: 'absolute', left: 0, right: 0, top: (full ? 0 : BAR) + 18, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 16, paddingVertical: 9, borderRadius: 18, backgroundColor: 'rgba(0,0,0,0.72)' }}>
            <Text style={{ fontSize: 13, fontWeight: 600, color: '#f2f2f5' }}>{tr('machines.capture.notice')}</Text>
          </View>
        </View>
      ) : null}
      {/* captured, the pointer can't reach the floating bar (and the guest's pointer at the top edge mustn't open it) */}
      {pip ? <PipBack name={name} onBack={fromPip} />
        : appsActive || (full && capOn) ? null : full ? <FloatingBar ctl={ctl} name={name} /> : <View style={{ position: 'absolute', left: 0, right: 0, top: 0 }}><Header ctl={ctl} name={name} /></View>}
      {cfgSheet ? <Sheet width={1000} height={700} modal onClose={() => setCfgSheet(null)}>
        <Configure dir={dir} section={cfgSheet.section} onClose={() => setCfgSheet(null)} />
      </Sheet> : null}
    </View>
  );
}
