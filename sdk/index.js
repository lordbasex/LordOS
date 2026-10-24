// @lordos/sdk — everything a LordOS theme or app needs.
export { standaloneDesktop } from './platform.js';
import { createElement, cloneElement, Fragment } from 'react';
import { render, onScreenChange, addListener } from '@lordos/runtime';
import { settings } from './settings.js';
import { createStore, useStore } from './store.js';
import { locale, tOptional } from './i18n.js';

export { View, Text, Image, Terminal, Surface, MediaView, screen, addListener, useClock, system, layouts, getLayout, modifiers } from '@lordos/runtime';
export { displays as screenDisplays, mainDisplay, displayAt, displayFor, mainBox } from '@lordos/runtime';
export { createStore, useStore } from './store.js';
export { settings, useSettings } from './settings.js';
export { t, tOptional, useLocale, locale, languages, appName, formatDate, formatTime, formatNumber, weekdayName, monthName, setLanguage, reloadCatalogs, direction, textDirection } from './i18n.js';
export { keyboard } from './keyboard.js';
export { trackpad, useTrackpads, hasTouchpad, registerGestureActions, lastSwipe, TRACKPAD_DEFAULTS, trackpadConfig, setTrackpad } from './trackpad.js';
export { ime, useIme } from './ime.js';
export { wayland } from './wayland.js';
export { useLauncherApps, launchApp, refreshApps, isLinuxApp, linuxAppFor, shortAppName, windowAppName, LetterIcon, iconFromFile, APP_CATEGORIES, appCategory } from './apps.js';
import { windowIcon as windowIconOf } from './apps.js';
import { ImeOverlay } from './ime.js';
import { ClipboardPanel, ClipboardToast, clipboard as clipboardApi } from './clipboard.js';
import { SpotlightPanel } from './spotlight.js';
import { RemoteBadge } from './remote.js';
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
import { askPower, canPower } from './power.jsx';
export { PowerDialog, powerAsk, POWER_SECONDS } from './power.jsx';
import { EmojiPanel } from './emoji.jsx';
export { admin } from './admin.js';
export { videoPrefs, setVideoPrefs, videoHardware, videoError, videoOptimizing, VIDEO_MODES, VIDEO_DEFAULTS } from './video.js';   // docs/VIDEO.md
import { startVideo } from './video.js';
export { browserFrameResult, browserFrameState, browserTabsInTitlebar, setBrowserTabsInTitlebar } from './browserframe.js';   // docs/WAYLAND.md
import { startBrowserFrame } from './browserframe.js';
export { errorText, statusText, codeError, parseCode } from './errors.js';
export { logs } from './logs.js';
export { LordOSLogo, LordOSGlyph, LordOSWordmark } from './logo.js';
export { Lumo, lumoSource } from './mascot.js';
export { DesktopWidgets, registerWidgetType, setWidgets, desktopWidgets } from './widgets.js';
export { Markdown, markdownHeadings, parseMarkdown, parseInline, toggleTask } from './markdown.jsx';
export {
  dock, useDockEntries, dockActivate, dockMenu, registerDockMenu, registerDockActions, dockQuickActions, launcherIdOf, dockIdOf, dockIdOfWindow,
  launchDockItem, dockPrefs, DOCK_DEFAULTS, quitApp, DOCK_LAUNCHER, useTrash, trashState, refreshTrash, openTrash, emptyTrash, trashMenu,
  dockDrag, dockPoof, beginDockDrag, registerDockTarget, dockDragJustEnded,
} from './dock.js';
export { sound, useSound, useSoundPeak, soundState, defaultSink, defaultSource, microphones } from './sound.js';
export { monitor, useMonitor, monitorSamples, monitorProcesses, monitorConnections, monitorDevices, monitorDetail, MONITOR_HISTORY, diskTempHistory, DISK_TEMP_HISTORY, firewall, firewallState } from './monitor.js';
export * as fwRule from './fwrule.js';            // checks what the user types for a firewall rule
export { packages, packageList, refreshPackages, packageFile, loadWasm } from './packages.js';
import { refreshPackages } from './packages.js';
export {
  windows, wm, openedAt, useWindows, configureWindows, getApp, listApps, isLiveSystem, appIdOf, workArea, workAreaOf, pointerWorkArea, appMenus, useAppMenus, windowTitle, appRegistry, appMenuClick,
  registerBuiltinApp, isSystemApp, snapPreview, snapRect, snapZoneAt, snapEnabled, SNAP_ZONES,
} from './windows.js';
export { codeStatus } from './codesig.js';
import { reportRefusedCode } from './codesig.js';
export { WindowShell, WINDOW_EDGE } from './windowshell.jsx';
export { term } from './terminal.js';
export { clipboard, clipboardHistory, clipboardPanel, clipboardKind } from './clipboard.js';
export { spotlight, spotlightOpen, calc as spotlightCalc, convert as spotlightConvert } from './spotlight.js';
export { registerSpotlightSource, searchEverything } from './spotlight.js';
// session notifications, menu bar status items, Clock (alarms) and Help (docs/SYSTEM.md)
export { notify, dismissNotification, useNotifications, notifications, notifyHistory, clearNotifyHistory, NotificationBanner, setBannerPlacement } from './notify.js';
export { registerStatusItem, StatusItems, statusItems } from './statusitems.js';
export { clockAlarms, clockTimer, timerStart, timerPause, timerResume, timerReset, timerLeft, stopwatch, swStart, swStop, swLap, swReset, swElapsed, nextAlarm, nextFire } from './alarms.js';
export { help } from './help.js';
import { NotificationBanner } from './notify.js';
import { startAlarms } from './alarms.js';
import { startDiskWatch } from './diskwatch.js';
import { maybeStartTour } from './help.js';
import { getApp as getAppOf, isLiveSystem as isLiveSystemOf, wm as wmOf, windows as windowsStore } from './windows.js';
export const windowIcon = (w) => windowIconOf(w, getAppOf);
export { systemInfo, lordosVersion, useSystemInfo, useServerStats, useCpuUsage, formatBytes, formatDuration } from './system.js';
export * as fs from './fs.js';
export { useDirectory } from './fs.js';
export { ThemeProvider, useTheme, DEFAULT_TOKENS } from './theme.js';
export {
  Button, Toggle, Segmented, SidebarItem, SectionTitle, ScrollView, TextInput, hasTextFocus, takeTextFocus, releaseTextFocus, textFocusOwner, Stepper, Slider,
  AppHost, useAppWindow, useKeyboard, focusToken, restoreFocus, pasteIntoFocused, FloatingWindows, useWindowState,
} from './components.jsx';
import { FloatingWindows } from './components.jsx';
// One stacking order for the desktop: <Layer name>, <Overlay anchor> (docs/LAYERS.md)
export { LAYERS, Layer, Overlay, shellLayers } from './layers.js';
import { Layer, enableShellLayers } from './layers.js';
export {
  ToolbarButton, ToolbarGroup, Toolbar, ToolbarSpacer, ToolbarDivider, Hairline, MenuButton, SearchField, EmptyState, GroupBox, FormRow, spacing, typeScale, controlSize, ConfirmDialog,
} from './controls.jsx';
export { media, useMedia, usePlayer, nowPlaying, useNowPlaying, NowPlayingTile, mediaFormatTime, thumbFor, useThumb, userDirs } from './media.js';
export { disks, useDisks } from './disks.js';
export { smart, smartSummaries, useSmartSummaries, diskTempLimit, diskAlertPrefs, setDiskAlerts } from './smart.js';
export { parseSmartctl, prettyAttrName, ATTR_KEYS, HEALTH_RANK } from './smartparse.js';
export { backup, useBackup, backupStatus } from './backup.js';   // Backups (docs/SYSTEM.md)
export { lordosUpdate, restartNeeded, updatePrefs, setUpdatePrefs, UPDATE_DEFAULTS } from './updates.js';   // updating LordOS in place (docs/UPDATES.md)
// Store → Discover, "Suggested for your computer": packages for the detected hardware (docs/PACKAGES.md)
export { suggestions, refreshSuggestions, reevaluate as reevaluateSuggestions, dismissSuggestion, markSuggestionsSeen, unseenSuggestions } from './suggestions.js';
export { vaultAvailable, vaultRequest, onVaultEvent, keyring, autofill, autofillReveal, KeyringPrompt, AutofillButton } from './vault.js';   // Passwords and keyring (docs/PASSWORDS.md)
import { KeyringPrompt } from './vault.js';
// Mail, Calendar, Contacts and Reminders through the sandboxed helper lord-maild (docs/MAIL.md)
export { mailAvailable, mailRequest, onMailEvent, mailConfig, mailAccounts, mailStatus, autoconfig, OAUTH_PROVIDERS, trackMailStatus, isolatedPim, pickFileFd, closeFileFd } from './mail.js';
export { calendars, events as calendarEvents, reminders, usePim, usePimQuery, pimRev, startOfDay } from './calendar.js';
export { FilePicker } from './filepicker.jsx';
export { openFileWithApps, revealPath } from './sysopen.js';   // opening files/apps for the isolated system apps
export { books as addressBooks, contacts, displayName as contactName, initials as contactInitials, ContactPhoto } from './contacts.js';
import { startMail } from './mail.js';
import { startReminders } from './calendar.js';
// Settings' hardware pages (docs/SETTINGS.md): displays, Night Light, battery, Bluetooth, idle, clock
export {
  displays, useDisplays, applyDisplay, keepDisplay, revertDisplay, arrangeDisplay, setDisplayMirror, setMainDisplay, setCaptureAllDisplays, displayConfirm, DisplayGlyph, nightPrefs, NIGHT_DEFAULTS, nightWindow,
  nightActive, nightAmount, setNightManual, previewNight, nightTick, sunTimes, battery, useBattery, BatteryGlyph, powerPrefs, POWER_DEFAULTS,
  powerProfiles, usePowerProfiles, onPowerProfileResult, bluetooth, useBluetooth, onBluetoothResult, timedate, useTimedate, menuClock, clockPrefs,
  CLOCK_DEFAULTS, applyIdle,
} from './hardware.jsx';
import { DisplayConfirm, BluetoothPrompt, startHardware } from './hardware.jsx';
// Fans and cooling (lord-fand, docs/FANS.md): Settings → Cooling and Monitor → Cooling
export { useFan, fanStatus, fanHistory, fanCase, fanControl, refreshFan, coolingVariant, heatColor, CoolingArt, FanCurveChart, formatTemp, FAN_HISTORY } from './cooling.jsx';
export { DEFAULT_CATEGORIES, defaultFor, setDefaultFor, candidatesFor, defaultOpenerId, nativeOf } from './defaults.js';
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
          frame: { x: w.x, y: w.y, w: w.w, h: w.h }, maximized: !!w.maximized }));
    } catch (e) { reopen = []; }
    settings.set({ theme: id, themeReopen: reopen.length ? reopen : null });
    lord.settings.save(JSON.stringify(settings.get(), null, 2));
    return lord.themes.use(id);
  },
};

// Session: log out, lock the screen or go back to the login screen so another
// user can sign in without closing this one (docs/SESSIONS.md). Locking and switching user need
// lord-sessiond (managed()); in a session viewed only from the browser, locking closes the
// browser's access.
export const session = {
  quit: () => { clipboardApi.beforeLogout(); lord.quit(); },
  managed: () => typeof lord.session !== 'undefined' && lord.session.managed(),
  lock: () => typeof lord.session !== 'undefined' && lord.session.lock(),
  switchUser: () => typeof lord.session !== 'undefined' && lord.session.switchUser(),
  // Restart… and Shut Down… (0.8.1): a confirmation with a 60 s countdown (sdk/power.jsx), then
  // lord-sessiond decides. canPower(): only in a session lord-sessiond shows on the monitor.
  canPower,
  restart: () => askPower('reboot'),
  shutdown: () => askPower('poweroff'),
};

// ❖L or ❖Ctrl+Q lock the screen.
addListener('keydown', (e) => {
  const k = String(e.base || e.key || '').toLowerCase();
  if (e.meta && !e.alt && !e.repeat && ((k === 'l' && !e.ctrl && !e.shift) || (k === 'q' && e.ctrl)) && session.managed()) session.lock();
});

// The screen size changed: everything is redrawn with the new measurements.
const screenRev = createStore(0);
onScreenChange(() => screenRev.set((v) => v + 1));

// Entry point of a theme: respects the theme chosen by the user.
export function startShell(element) {
  const wanted = settings.get().theme;
  const current = themes.current();
  if (wanted && current && wanted !== current && lord.themes.use(wanted)) return;
  refreshPackages();                    // after the theme's configureWindows
  enableShellLayers();                  // menus, popovers and prompts go to their layer (sdk/layers.js)
  reopenAfterThemeSwitch();
  render(createElement(LocaleRoot, null, element));
  // live ISO: the installation assistant opens by itself, in front
  if (isLiveSystemOf() && getAppOf('installer')) setTimeout(() => wmOf.open('installer'), 300);
  startAlarms();                        // Clock alarms and timer, with the app closed
  startMail();                          // new-mail notifications and passwords on demand (docs/MAIL.md)
  startReminders();                     // calendar and reminder alarms, with the apps closed
  maybeStartTour();                     // "Welcome to LordOS", the first time
  startHardware();                      // Night Light, idle timers, battery in the menu bar (hardware.jsx)
  startDiskWatch();                     // notices when a disk gets too hot or its health gets worse
  startVideo();                         // browser video without hardware decoding (video.js, docs/VIDEO.md)
  startBrowserFrame();                  // "Browsers: tabs in the title bar" left for Chromium to quit (browserframe.js)
  setTimeout(reportRefusedCode, 800);   // "This LordOS component was modified and was not loaded" (docs/SIGNING.md)
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
    const id = wmOf.open(r.app, { ...(inst ? { instance: inst } : null), ...(r.args ? { args: r.args } : null), frame: r.frame });
    if (id && r.maximized) wmOf.toggleMaximize(id);
    if (id && r.minimized) wmOf.minimize(id);
  }
  // apps that bring their own windows back as they start (the Terminal's sessions) must not end up
  // in front of the one the user was using
  const top = [...list].reverse().find((r) => r && !r.minimized && getAppOf(r.app));
  if (top) setTimeout(() => { if (windowsStore.get().some((w) => w.id === top.id)) wmOf.focus(top.id); }, 900);
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
    at('overlay', SpotlightPanel),
    at('popover', ClipboardPanel, EmojiPanel),
    at('modal', PrintDialog),
    at('notification', NotificationBanner, ClipboardToast, RemoteBadge),
    at('system', PermissionPrompt, Powerbox, KeyringPrompt, BluetoothPrompt, DisplayConfirm),
    at('input', ImeOverlay));
}
export { lordosWallpapers, wallpaperSrc, wallpaperThumb, findWallpaper, DEFAULT_WALLPAPER, GREETER_WALLPAPER, WALLPAPER_CATEGORIES } from './wallpapers.js';
