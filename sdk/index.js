// @lordos/sdk — everything a LordOS theme or app needs.
import { createElement, cloneElement, Fragment } from 'react';
import { render, onScreenChange } from '@lordos/runtime';
import { settings } from './settings.js';
import { createStore, useStore } from './store.js';
import { locale, tOptional } from './i18n.js';

export { View, Text, Image, Terminal, Surface, MediaView, screen, addListener, useClock, system, layouts, getLayout, modifiers } from '@lordos/runtime';
export { displays as screenDisplays, mainDisplay, displayAt, displayFor, mainBox } from '@lordos/runtime';
export { createStore, useStore } from './store.js';
export { settings, useSettings } from './settings.js';
export { t, tOptional, useLocale, locale, languages, preferredLanguages, setPreferredLanguages, CORE_LANGUAGES, appName, formatDate, formatTime, formatNumber, weekdayName, monthName, setLanguage, reloadCatalogs, direction, textDirection } from './i18n.js';
export { keyboard } from './keyboard.js';
export { regionOf, localeOf, regionCountry } from './localename.js';   // glibc region names (installer, Settings)
export { LANGUAGE_NAMES } from './langnames.js';   // languages not translated yet (Settings → Language & Region)
export { useShortcuts, addShortcuts, runShortcut, shortcutList, shortcutsRev, shortcutTaken } from './shortcuts.js';
export { ContextMenu } from './contextmenu.jsx';
export { liveSpeech } from './livespeech.jsx';
export { speak, stopSpeaking, pauseSpeaking, replaySpeaking, closePlayer, saveSpeech, speaking, player, speakSelection, speakAvailable, registerSelection, speechMenu, speechItems, useMenusWithSpeech, voiceSettings, VOICE_DEFAULTS, previewVoice, tryVoice } from './voice.js';
export { VOICE_LANGS, VOICES, voiceLabel, voicePackage, modelPackage, hasVoice, PREVIEWS_PACKAGE } from './voicecatalog.js';
export { parseWav, levelDb, quietEnough, roomCheck, takeName, trimSilence, joinPhrases, phraseDone } from './voicerecmodel.js';
export { scanVoices, installVoices, removeVoices, packagesFor, voiceConf, setVoiceConf, addOwnVoice, removeOwnVoice, ownVoiceName } from './voicemanager.js';
import { resetShortcuts } from './shortcuts.js';
export { AppSwitcher, openSwitcher, cycleAppWindows, switcherKeys, switcherGrabs, switcherHold } from './appswitcher.jsx';
export { switcherPrefs, SWITCHER_DEFAULTS, validColor } from './switchermodel.js';
export { RemoteDeskApp } from './remotedesk.jsx';
export { osk, oskState, startOsk } from './osk.js';
export { OskPanel, OskIcon, OskStatus, oskLift } from './osk.jsx';
export { trackpad, useTrackpads, hasTouchpad, registerGestureActions, lastSwipe, TRACKPAD_DEFAULTS, trackpadConfig, setTrackpad } from './trackpad.js';
export { ime, useIme } from './ime.js';
export { wayland } from './wayland.js';
export { useLauncherApps, launchApp, refreshApps, isLinuxApp, linuxAppFor, shortAppName, windowAppName, LetterIcon, iconFromFile, APP_CATEGORIES, appCategory } from './apps.js';
import { windowIcon as windowIconOf } from './apps.js';
import { ImeOverlay } from './ime.js';
import { startOsk } from './osk.js';
import { startWindowMemory } from './windowmemory.js';
import { OskPanel, OskStatus } from './osk.jsx';
import { registerStatusItem } from './statusitems.js';
import { ClipboardPanel, ClipboardToast, clipboard as clipboardApi } from './clipboard.js';
import { SpotlightPanel } from './spotlight.js';
import { RemoteBadge } from './remote.js';
import { ShareBadge, SharePrompt, sweepShareGrants } from './share.js';
import { linuxIds } from './apps.js';
export { share, shareHolder, shareRev } from './share.js';
export { remote, remoteViewers, sshLogin, remoteCidr, remoteMinutesLeft, remoteAccess, remoteVideo, remoteIceServer, remoteFingerprint } from './remote.js';
export { perms, permsRev, requestPermission, markDeviceUse, useIndicators, PrivacyIndicators, PermissionPrompt, registerPrivacySection, privacySections, PERMISSIONS, onPermissionRevoked, micStart, micStop, micLevel } from './privacy.js';
import { PermissionPrompt } from './privacy.js';
import { Powerbox } from './powerbox.jsx';
export { openFile, saveFile, Powerbox } from './powerbox.jsx';
// documents and printing (M9): PDF, archives, fonts, print dialog and emoji
export { docAvailable, DocImage, releaseImage, releaseBlob, saveBlob, fontInfo, fontSample, fontGrid, decodeImage, convertImage, makeTextPdf, makeImagePdf, pdfThumb, warmDocs } from './doc.js';
export { pdf } from './pdf.js';
export { archive } from './archive.js';
export { print, queueName } from './print.js';
export { emoji, Grid as EmojiGrid, ToneBar as EmojiToneBar } from './emoji.jsx';
import { PrintDialog } from './printdialog.jsx';
import { RestorePrompt, startSessionRestore, markThemeSwitch, logOut } from './sessionrestore.jsx';
import { askPower, canPower } from './power.jsx';
export { PowerDialog, powerAsk, POWER_SECONDS } from './power.jsx';
import { EmojiPanel } from './emoji.jsx';
export { admin } from './admin.js';
export { videoPrefs, setVideoPrefs, videoHardware, videoError, videoOptimizing, VIDEO_MODES, VIDEO_DEFAULTS } from './video.js';   // docs/VIDEO.md
import { startVideo } from './video.js';
export { browserFrameResult, browserFrameState, browserTabsInTitlebar, setBrowserTabsInTitlebar } from './browserframe.js';   // docs/WAYLAND.md
import { startBrowserFrame } from './browserframe.js';
import { startNetStatus } from './netstatus.jsx';
import { startCleanerStatus } from './cleanerstatus.jsx';
import { startCloud } from './cloudstatus.jsx';
export { errorText, statusText, codeError, parseCode } from './errors.js';
export { logs } from './logs.js';
export { LordOSLogo, LordOSGlyph, LordOSWordmark } from './logo.js';
export { Lumo, lumoSource } from './mascot.js';
export { DesktopWidgets, registerWidgetType, setWidgets, desktopWidgets } from './widgets.js';
export { Markdown, markdownHeadings, parseMarkdown, parseInline, toggleTask } from './markdown.jsx';
export {
  dock, useDockEntries, dockActivate, dockMenu, registerDockMenu, registerDockActions, dockQuickActions, launcherIdOf, dockIdOf, dockIdOfWindow,
  launchDockItem, dockPrefs, DOCK_DEFAULTS, dockHideSettings, DOCK_HIDE_MODES, useDockCovered, quitApp, DOCK_LAUNCHER, useTrash, trashState, refreshTrash, openTrash, emptyTrash, trashMenu,
  dockDrag, dockPoof, beginDockDrag, registerDockTarget, dockDragJustEnded,
} from './dock.js';
export { sound, useSound, useSoundPeak, soundState, defaultSink, defaultSource, microphones } from './sound.js';
export { SoundOutputList, useSoundOutputs } from './soundoutputs.js';   // choosing the output by the volume (issue #58)
export { monitor, useMonitor, monitorSamples, monitorProcesses, monitorConnections, monitorDevices, monitorDetail, MONITOR_HISTORY, diskTempHistory, DISK_TEMP_HISTORY, firewall, firewallState } from './monitor.js';
export * as fwRule from './fwrule.js';            // checks what the user types for a firewall rule
export * as netModel from './netmodel.js';        // (0.8.5) Monitor → Network: search tokens, filters, the tree
export { packages, packageList, refreshPackages, packageFile, loadWasm } from './packages.js';
import { refreshPackages, forgetGonePackaged, packageList as packageListOf } from './packages.js';
export {
  windows, wm, openedAt, useWindows, configureWindows, getApp, listApps, isLiveSystem, appIdOf, workArea, workAreaOf, pointerWorkArea, appMenus, useAppMenus, windowTitle, appRegistry, appMenuClick,
  registerBuiltinApp, isSystemApp, snapPreview, snapRect, snapZoneAt, snapEnabled, SNAP_ZONES, useWindowHidden, activeDesktop, desktopList, desktopNames, renameDesktop, moveDesktop,
  keepWindowsInWorkAreas, pointerDisplay,
} from './windows.js';
// Desktops and the Overview (docs/DESKTOPS.md)
export { desktopName, nextDesktopBy, addDesktopAndShow, closeActiveDesktop, moveToDesktopItems, flowDesktopKeys, classicDesktopKeys, flowGrabs, ctrlArrowsOn, desktopSwipe, DesktopHud } from './desktops.js';
import { startDesktops, desktopsForThemeSwitch, desktopAt, indexOfDesktop } from './desktops.js';
// the brief level indicator of the volume, brightness and keyboard-light keys (docs/SHORTCUTS.md)
export { LevelHud, showLevel } from './levelhud.js';
export { overview, OverviewBackdrop, OverviewChrome, openOverview, closeOverview, toggleOverview, toggleAppWindows, overviewOpen } from './overview.jsx';
export { codeStatus } from './codesig.js';
import { reportRefusedCode } from './codesig.js';
export { WindowShell, WINDOW_EDGE } from './windowshell.jsx';
export { term } from './terminal.js';
export { clipboard, clipboardHistory, clipboardPanel, clipboardKind, toast } from './clipboard.js';
export { restoreMode, setRestoreMode, restoreBrowsers, setRestoreBrowsers } from './sessionrestore.jsx';
export { spotlight, spotlightOpen, calc as spotlightCalc, convert as spotlightConvert } from './spotlight.js';
export { registerSpotlightSource, searchEverything } from './spotlight.js';
// session notifications, menu bar status items, Clock (alarms) and Help (docs/SYSTEM.md)
export { notify, dismissNotification, useNotifications, notifications, notifyHistory, clearNotifyHistory, NotificationBanner, setBannerPlacement } from './notify.js';
export { registerStatusItem, StatusItems, statusItems } from './statusitems.js';
export { MENUBAR_DEFAULTS, menuBarPrefs, menuBarPatch, cleanerItemShown, outputChoices, monthCells, weekStartFor } from './menubarmodel.js';   // what the bar shows (issue #58)
export { menuBarEveryDisplay, barDisplays, hasBar, displayIdFor, activeBarId, pinBar, focusDisplayId, panelLeft, inBar, insideDisplay } from './menubarmodel.js';   // a bar on every display
export { sharedTicker, idleStore } from './ticker.js';
export { companion, statusItem, companionEvent, statusItemEvent } from './companion.js';
export { clockAlarms, clockTimer, timerStart, timerPause, timerResume, timerReset, timerLeft, stopwatch, swStart, swStop, swLap, swReset, swElapsed, nextAlarm, nextFire } from './alarms.js';
export { help } from './help.js';
import { NotificationBanner } from './notify.js';
import { startAlarms } from './alarms.js';
import { startDiskWatch } from './diskwatch.js';
import { maybeStartTour } from './help.js';
import { liveStartWindow } from './livestart.js';
import { getApp as getAppOf, isLiveSystem as isLiveSystemOf, wm as wmOf, windows as windowsStore } from './windows.js';
export const windowIcon = (w) => windowIconOf(w, getAppOf);
export { systemInfo, systemReport, lordosVersion, useSystemInfo, useServerStats, useCpuUsage, formatBytes, formatDuration } from './system.js';
export * as fs from './fs.js';
export { useDirectory } from './fs.js';
export { ThemeProvider, useTheme, DEFAULT_TOKENS, solidOver } from './theme.js';
export {
  Button, Toggle, Segmented, SidebarItem, SectionTitle, ScrollView, TextInput, FormFields, hasTextFocus, takeTextFocus, releaseTextFocus, textFocusOwner, Stepper, Slider,
  AppHost, useAppWindow, useKeyboard, focusToken, restoreFocus, pasteIntoFocused, FloatingWindows, useWindowState,
} from './components.jsx';
import { FloatingWindows } from './components.jsx';
// One stacking order for the desktop: <Layer name>, <Overlay anchor> (docs/LAYERS.md)
export { LAYERS, Layer, Overlay, shellLayers } from './layers.js';
import { Layer, enableShellLayers } from './layers.js';
export {
  ToolbarButton, ToolbarGroup, Toolbar, ToolbarSpacer, ToolbarDivider, Hairline, MenuButton, SearchField, EmptyState, GroupBox, FormRow, spacing, typeScale, controlSize, ConfirmDialog,
} from './controls.jsx';
export { media, useMedia, usePlayer, nowPlaying, useNowPlaying, NowPlayingTile, mediaFormatTime, thumbFor, useThumb, userDirs, openMarkup } from './media.js';
export * as imageEdit from './imageeditmodel.js';   // the image editor's math (crop presets, resize, redaction; issue #30)
export { disks, useDisks, needsDiskFormats, openDiskFormats } from './disks.js';
export { lan } from './lan.js';                                  // Files → Network (docs/FILES.md)
export { smart, smartSummaries, useSmartSummaries, diskTempLimit, diskAlertPrefs, setDiskAlerts } from './smart.js';
export { parseSmartctl, prettyAttrName, ATTR_KEYS, HEALTH_RANK } from './smartparse.js';
export { backup, useBackup, backupStatus } from './backup.js';   // Backups (docs/SYSTEM.md)
export { cloud, useCloud, cloudState } from './cloud.js';        // Cloud (docs/CLOUD.md)
export * as cloudModel from './cloudmodel.js';
export { CloudGlyph, StatusGlyph, StatusBadge, UsageRing, AccountBadge, SpaceBar } from './cloudui.jsx';
export { cleaner } from './cleaner.js';   // Cleaner (docs/CLEANER-ARCHITECTURE.md)
export { statusWord } from './cleanerstatus.jsx';
export { cleanerPalette, CLEANER_THEMES } from './cleanertheme.js';
export { oldKernels, kernelPlanOk } from './oldkernels.js';
export { lordosUpdate, restartNeeded, updatePrefs, setUpdatePrefs, UPDATE_DEFAULTS } from './updates.js';   // updating LordOS in place (docs/UPDATES.md)
// Store → Discover, "Suggested for your computer": packages for the detected hardware (docs/PACKAGES.md)
export { suggestions, refreshSuggestions, reevaluate as reevaluateSuggestions, dismissSuggestion, markSuggestionsSeen, unseenSuggestions } from './suggestions.js';
export { vaultAvailable, vaultRequest, onVaultEvent, keyring, autofill, autofillReveal, KeyringPrompt, AutofillButton, askKeyringUnlock } from './vault.js';   // Passwords and keyring (docs/PASSWORDS.md)
import { KeyringPrompt } from './vault.js';
// Mail, Calendar, Contacts and Reminders through the sandboxed helper lord-maild (docs/MAIL.md)
export { mailAvailable, mailRequest, onMailEvent, mailConfig, mailAccounts, mailStatus, autoconfig, OAUTH_PROVIDERS, DAV_PRESETS, davPreset, openDavSetup, openMailAccounts, trackMailStatus, isolatedPim, pickFileFd, closeFileFd } from './mail.js';
export { calendars, events as calendarEvents, reminders, usePim, usePimQuery, pimRev, startOfDay } from './calendar.js';
export { FilePicker } from './filepicker.jsx';
import { PortalFilePrompt } from './filepicker.jsx';
import { VoicePlayer, VoiceSavePrompt } from './voiceplayer.jsx';
import { LiveSpeechPanel } from './livespeech.jsx';
export { openFileWithApps, openUrl, revealPath, openCalendarApp } from './sysopen.js';   // opening files/apps for the isolated system apps
export { books as addressBooks, contacts, displayName as contactName, initials as contactInitials, ContactPhoto } from './contacts.js';
import { startMail } from './mail.js';
import { startReminders } from './calendar.js';
// Settings' hardware pages (docs/SETTINGS.md): displays, Night Light, battery, Bluetooth, idle, clock
export {
  displays, useDisplays, applyDisplay, keepDisplay, revertDisplay, arrangeDisplay, setDisplayMirror, setMainDisplay, setCaptureAllDisplays, setAutoRotateDisplays, displayConfirm, DisplayGlyph, nightPrefs, NIGHT_DEFAULTS, nightWindow,
  nightActive, nightAmount, setNightManual, previewNight, nightTick, sunTimes, battery, useBattery, BatteryGlyph, powerPrefs, POWER_DEFAULTS,
  powerProfiles, usePowerProfiles, onPowerProfileResult, bluetooth, useBluetooth, onBluetoothResult, timedate, useTimedate, menuClock, clockPrefs,
  CLOCK_DEFAULTS, applyIdle,
} from './hardware.jsx';
import { DisplayConfirm, BluetoothPrompt, PolkitPrompt, startHardware } from './hardware.jsx';
// Settings → Displays: numbering, Identify (a big number on each real display) and magnetic arrangement
export { identifyDisplays, identifying as identifyingDisplays, IDENTIFY_MS } from './displaysui.jsx';
import { DisplayIdentify } from './displaysui.jsx';
export { displayNumbers, snapDisplay, resolveDrop, placementOk as displayPlacementOk, SNAP_PREVIEW_PX, scaleChoices, scaleTiles } from './displaylayout.js';
// Fans and cooling (lord-fand, docs/FANS.md): Settings → Cooling and Monitor → Cooling
export { useFan, fanStatus, fanHistory, fanCase, fanControl, refreshFan, coolingVariant, heatColor, CoolingArt, FanCurveChart, formatTemp, FAN_HISTORY } from './cooling.jsx';
export { DEFAULT_CATEGORIES, defaultFor, defaultInfo, setDefaultFor, resetDefaultFor, candidatesFor, defaultOpenerId, nativeOf } from './defaults.js';
export { notifyPrefs, appNotifyPrefs, setAppNotifyPrefs, dndActive, NOTIFY_DEFAULTS } from './notify.js';

// ---- installed themes ----

export const themes = {
  list: () => lord.themes.list().map((t) => {
    let meta = {};
    try { meta = t.meta ? JSON.parse(t.meta) : {}; } catch (e) { /* invalid theme.json */ }
    // translated name and description (shell.theme.<id>.*) or those from theme.json
    return { id: t.id, current: t.current, hidden: !!meta.hidden, name: tOptional(`shell.theme.${t.id}.name`) || meta.name || t.id,
      description: tOptional(`shell.theme.${t.id}.description`) || meta.description || '' };
  }).filter((t) => !t.hidden),         // the login screen (themes/greeter) can't be chosen
  current: () => lord.themes.list().find((t) => t.current)?.id || null,
  // Switches theme live: the server discards this JS context and loads the new one. The LordOS
  // app windows that are open come back in the new theme (themeReopen, read once by startShell);
  // Linux app windows stay connected and simply get the new theme's frame.
  use(id) {
    let reopen = [];
    try {
      reopen = windowsStore.get().filter((w) => { const a = getAppOf(w.id); return a && !a.builtin; }).slice(-8)
        .map((w) => ({ app: w.app, id: w.id, args: w.args ? JSON.parse(JSON.stringify(w.args)) : null, minimized: !!w.minimized,
          frame: { x: w.x, y: w.y, w: w.w, h: w.h }, maximized: !!w.maximized, desk: indexOfDesktop(w.desktop) }));
    } catch (e) { reopen = []; }
    markThemeSwitch();
    settings.set({ theme: id, themeReopen: reopen.length ? reopen : null, themeDesktops: desktopsForThemeSwitch() });
    lord.settings.save(JSON.stringify(settings.get(), null, 2));
    return lord.themes.use(id);
  },
};

// Session: log out, lock the screen or go back to the login screen so another
// user can sign in without closing this one (docs/SESSIONS.md). Locking and switching user need
// lord-sessiond (managed()); in a session viewed only from the browser, locking closes the
// browser's access.
export const session = {
  quit: logOut,
  managed: () => typeof lord.session !== 'undefined' && lord.session.managed(),
  lock: () => typeof lord.session !== 'undefined' && lord.session.lock(),
  switchUser: () => typeof lord.session !== 'undefined' && lord.session.switchUser(),
  // Restart… and Shut Down… (0.8.1): a confirmation with a 60 s countdown (sdk/power.jsx), then
  // lord-sessiond decides. canPower(): only in a session lord-sessiond shows on the monitor.
  canPower,
  restart: () => askPower('reboot'),
  shutdown: () => askPower('poweroff'),
};

// The screen size changed: everything is redrawn with the new measurements.
const screenRev = createStore(0);
onScreenChange(() => screenRev.set((v) => v + 1));

// Entry point of a theme: respects the theme chosen by the user.
export function startShell(element) {
  startDesktops();
  const wanted = settings.get().theme;
  const current = themes.current();
  if (wanted && current && wanted !== current && lord.themes.use(wanted)) return;
  refreshPackages();                    // after the theme's configureWindows
  enableShellLayers();                  // menus, popovers and prompts go to their layer (sdk/layers.js)
  resetShortcuts();                     // the dispatcher exists only in the session (sdk/shortcuts.js)
  reopenAfterThemeSwitch();
  render(createElement(LocaleRoot, null, element));
  // live ISO: the installation assistant opens by itself, in front, or System Information for the
  // boot entry "Check this computer" (livestart.js)
  if (isLiveSystemOf()) {
    const opening = liveStartWindow(lord.system.info(), getAppOf);
    if (opening) setTimeout(() => wmOf.open(...opening), 300);
  }
  startAlarms();                        // Clock alarms and timer, with the app closed
  startMail();                          // new-mail notifications and passwords on demand (docs/MAIL.md)
  startReminders();                     // calendar and reminder alarms, with the apps closed
  maybeStartTour();                     // "Welcome to LordOS", the first time
  startHardware();                      // Night Light, idle timers, battery in the menu bar (hardware.jsx)
  startDiskWatch();                     // notices when a disk gets too hot or its health gets worse
  startVideo();                         // browser video without hardware decoding (video.js, docs/VIDEO.md)
  startBrowserFrame();                  // "Browsers: tabs in the title bar" left for Chromium to quit (browserframe.js)
  setTimeout(sweepUninstalledGrants, 5000); // a screen-share grant doesn't outlive its app (share.js)
  startCleanerStatus();                 // Cleaner's still item by the clock; its panel reads only while open
  startCloud();                         // Cloud's item by the clock (with an account) and its notifications (cloudstatus.jsx)
  startNetStatus();                     // (0.8.5) the menu bar's network item and the blocklists' daily update
  startOsk();                           // the on-screen keyboard (osk.js, docs/ON-SCREEN-KEYBOARD.md)
  registerStatusItem('osk', OskStatus, 6);
  watchPackaged();                      // an app dpkg installed or removed shows without a new session
  startWindowMemory();                  // each app reopens where it was (windowmemory.js, docs/WINDOWS.md)
  startSessionRestore();                // the apps that were open come back (sessionrestore.jsx, docs/SESSIONS.md)
  setTimeout(reportRefusedCode, 800);   // "This LordOS component was modified and was not loaded" (docs/SIGNING.md)
}

// A screen-share grant doesn't outlive its app, even one removed with dpkg behind the shell's back.
function sweepUninstalledGrants() {
  try { sweepShareGrants(new Set(packageListOf.get().map((p) => p.id)), linuxIds()); } catch (e) { /* next session */ }
  try { forgetGonePackaged(); } catch (e) { /* next session */ }
}

// A packaged app comes and goes with dpkg, behind the shell's back: one stat every few seconds, and only a change reads the list.
function watchPackaged() {
  if (typeof lord === 'undefined' || typeof lord.apps?.packagedStamp !== 'function') return;
  let last = lord.apps.packagedStamp();
  setInterval(() => {
    const now = lord.apps.packagedStamp();
    if (now === last) return;
    last = now;
    refreshPackages();
    sweepUninstalledGrants();
  }, 5000);
}

// After Settings → Appearance switched theme: the windows that were open replace the ones the new
// theme opens by itself at start.
function reopenAfterThemeSwitch() {
  const list = settings.get().themeReopen;
  if (!Array.isArray(list) || !list.length) return;
  settings.set({ themeReopen: null });
  for (const w of windowsStore.get()) if (!getAppOf(w.id)?.builtin) wmOf.close(w.id, true);
  for (const r of list) {
    if (!r || typeof r.app !== 'string' || !getAppOf(r.app)) continue;
    const inst = typeof r.id === 'string' && r.id.includes('#') ? r.id.split('#')[1] : null;
    const id = wmOf.open(r.app, { ...(inst ? { instance: inst } : null), ...(r.args ? { args: r.args } : null), frame: r.frame, desktop: desktopAt(r.desk) });
    if (id && r.maximized) wmOf.toggleMaximize(id);
    if (id && r.minimized) wmOf.minimize(id);
  }
  // apps that bring their own windows back as they start (the Terminal's sessions) must not end up
  // in front of the one the user was using
  const top = [...list].reverse().find((r) => r && !r.minimized && getAppOf(r.app));
  if (top) setTimeout(() => { if (windowsStore.get().some((w) => w.id === top.id)) wmOf.raise(top.id); }, 900);
}

// Changing the language remounts the whole interface (windows, tabs and
// sessions live in stores and are not lost).
function LocaleRoot({ children }) {
  const { lang, region } = useStore(locale);
  const rev = useStore(screenRev);
  // the SDK's own surfaces, each in its layer (sdk/layers.js); the theme's tree is the base
  const at = (name, ...kids) => createElement(Layer, { name }, ...kids.map((k, i) => createElement(k, { key: i })));
  return createElement(Fragment, { key: `${lang}|${region}` }, rev ? cloneElement(children, { screenRev: rev }) : children,
    at('floating', FloatingWindows),
    at('overlay', SpotlightPanel, LiveSpeechPanel),
    at('popover', ClipboardPanel, EmojiPanel),
    at('modal', PrintDialog, RestorePrompt),
    at('notification', NotificationBanner, ClipboardToast, RemoteBadge, ShareBadge, DisplayIdentify, VoicePlayer),
    at('system', PermissionPrompt, SharePrompt, Powerbox, KeyringPrompt, BluetoothPrompt, PolkitPrompt, PortalFilePrompt, VoiceSavePrompt, DisplayConfirm),
    at('input', OskPanel, ImeOverlay));
}
export { setLoginStill, setLoginImage, addWallpaperImage, removeWallpaperImage, MotionWallpaper, MOTION_SECTIONS, MOTION_RATES, motionBusy, motionItems, motionThumb, motionPoster, motionDownloads, useCodecs, downloadMotion, removeMotion, setLoginMotion, variantFor } from './motion.jsx';
export { userVideoProblem, motionCodecs, MAX_SECONDS } from './motionmodel.js';
export { lordosWallpapers, userWallpaperName, wallpaperSrc, wallpaperThumb, findWallpaper, DEFAULT_WALLPAPER, GREETER_WALLPAPER, WALLPAPER_CATEGORIES } from './wallpapers.js';
export { appGroup, appMenuItems, runningApps, forceQuit, recentItems, storeUpdates, hideApp, hideOthers, toggleShowDesktop, appSettingsItem } from './appcontrol.js';
export { windowMenuItems, moveResizeItems } from './windowmenu.js';
export { setDesktopCount } from './desktops.js';
