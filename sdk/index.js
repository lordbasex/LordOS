// @lordos/sdk — everything a LordOS theme or app needs.
import { createElement, cloneElement, Fragment } from 'react';
import { render, onScreenChange } from '@lordos/runtime';
import { settings } from './settings.js';
import { createStore, useStore } from './store.js';
import { locale, tOptional } from './i18n.js';
import { cleanThemeMeta, themeNamespace, themeText, themeNeedsNewer, THEME_TEXT_MAX } from './thememeta.js';
import { lordosVersion as lordosVersionOf } from './system.js';

export { View, Text, Image, Terminal, Surface, MediaView, screen, addListener, useClock, system, layouts, getLayout, modifiers } from '@lordos/runtime';
export { displays as screenDisplays, mainDisplay, displayAt, displayFor, mainBox, holdKeyboard } from '@lordos/runtime';
export { createStore, useStore } from './store.js';
export { cleanThemeMeta, themeOptions, themeOptionValue, themeOptionPatch, themeAuthor } from './thememeta.js';   // theme.json's sketch and options (0.8.9)
// (0.8.9.6.2) a community theme's own texts (theme-<id>) and the release it needs (minLordOS)
export { themeNamespace, themeText, themeLabel, lordosAtLeast, themeNeedsNewer, THEME_TEXT_MAX } from './thememeta.js';
export { settings, useSettings, useTrackedSettings } from './settings.js';
export { t, tOptional, useLocale, locale, languages, preferredLanguages, setPreferredLanguages, CORE_LANGUAGES, appName, formatDate, formatTime, formatNumber, weekdayName, monthName, setLanguage, reloadCatalogs, direction, textDirection } from './i18n.js';
export { keyboard } from './keyboard.js';
export { regionOf, localeOf, regionCountry, languageOfLocale, localeLanguage, regionLanguageCode, regionLabel } from './localename.js';   // glibc region names (installer, Settings)
export { LANGUAGE_NAMES } from './langnames.js';   // languages not translated yet (Settings → Language & Region)
export { useShortcuts, addShortcuts, runShortcut, shortcutList, shortcutsRev, shortcutTaken, addMenuBarKeys } from './shortcuts.js';
export { ContextMenu } from './contextmenu.jsx';
// spelling and ✨ AI on selected text (docs/SPELLING.md, docs/AI.md "Writing")
export { spellVersion, spellOn, spellLangs, spellInstalled, spellDefaults, spellRanges, useSpellRanges, spellSuggest, spellAddWord, spellRemoveWord, spellWordsList, spellIgnore } from './spell.js';
export { SPELL_PACKAGES, spellPackageOk, packageInstalled, toggleLang, spellLanguages, spellWords, badRanges } from './spellmodel.js';
export { useEditorSpell, EditorSquiggles, squiggleColorFor, spellHitAt, editorMenuRows } from './editorextras.jsx';
export { WritingPanel, writingReady, writingOn, setWritingOn, writingMenuFor } from './writing.jsx';
export { liveSpeech } from './livespeech.jsx';
export { speak, stopSpeaking, pauseSpeaking, replaySpeaking, closePlayer, saveSpeech, speaking, player, speakSelection, speakAvailable, registerSelection, speechMenu, speechItems, useMenusWithSpeech, voiceSettings, VOICE_DEFAULTS, previewVoice, tryVoice } from './voice.js';
export { VOICE_LANGS, VOICES, voiceLabel, voicePackage, modelPackage, hasVoice, PREVIEWS_PACKAGE } from './voicecatalog.js';
export { parseWav, levelDb, quietEnough, roomCheck, takeName, trimSilence, joinPhrases, phraseDone } from './voicerecmodel.js';
export { scanVoices, installVoices, removeVoices, packagesFor, voiceConf, setVoiceConf, addOwnVoice, removeOwnVoice, ownVoiceName } from './voicemanager.js';
// Sentinela, the process guard (docs/SENTINELA.md): its models, its rules and its session service
export { LLM_MODELS, ENGINE_PACKAGE, llmModel, packagesForModel, llmPackageOk } from './llmcatalog.js';
export { TRIGGERS, ACTIONS, SENTINELA_DEFAULTS, factLines, presentAlert, INVESTIGATE_ACTIONS, RISKS } from './sentinelamodel.js';
export { VM_ACTIONS, vmWhy, vmFactLines } from './sentinelavm.js';
export {
  sentinelaAlerts, sentinelaEngine, sentinelaPrefs, setSentinelaPrefs, refreshSentinelaEngine, runSentinelaAction, sentinelaOnBattery,
  clearSentinelaAlerts, actionLabel as sentinelaActionLabel, installLlm, removeLlm, lumoReady, investigateProcess, translateWithLumo,
  sentinelaBlocked, refreshBlocked, blockProgram, unblockProgram, runInvestigateAction,
} from './sentinela.js';
export { describeProgram, describeApp, saveLumoTranslation, translationKey } from './describe.js';
export { ShieldGlyph } from './sentinelaglyph.js';
import { resetShortcuts } from './shortcuts.js';
export { AppSwitcher, openSwitcher, cycleAppWindows, switcherKeys, switcherGrabs, switcherHold } from './appswitcher.jsx';
export { switcherPrefs, SWITCHER_DEFAULTS, validColor } from './switchermodel.js';
export { RemoteDeskApp } from './remotedesk.jsx';
export { osk, oskState, startOsk } from './osk.js';
export { OskPanel, OskIcon, OskStatus, oskLift } from './osk.jsx';
export { trackpad, useTrackpads, hasTouchpad, registerGestureActions, lastSwipe, TRACKPAD_DEFAULTS, trackpadConfig, setTrackpad } from './trackpad.js';
export { ime, useIme } from './ime.js';
export { wayland, x11SelfScale, setX11Scaling } from './wayland.js';
export { useLauncherApps, launchApp, refreshApps, guestLauncherApps, isLinuxApp, linuxAppFor, shortAppName, windowAppName, windowPixelIcons, LetterIcon, iconFromFile, webAppIcon, webProfileIcon, APP_CATEGORIES, appCategory } from './apps.js';
export { APP_GROUPS, launcherGroup, groupApps, startView, groupRows, groupKey, keepSelection } from './appgroupsmodel.js';   // Lord Classic's start menu by category
// Web apps (docs/WEBAPPS.md): the registry through lord.webapps, profiles, the probe, the permission questions
export { webApps, useWebApps, refreshWebApps, webAppsAvailable, webAppEntry, webRunning, launchWebApp, probeWebApp, installWebApp, webAppCall, webAppOfWindow, webAppMemory, webEngine,
  webBadges, siteIcons, wantSiteIcon, refreshSiteIcon, siteIconBusy, siteIconStatus, webDeveloper, setWebDeveloperMode, webSearchEngine, setWebSearchEngine,
  restartWebApp, quitWebApp, restartWebEngine } from './webapps.js';
export * as notifyTones from './notifytones.js';
export * as webAppsModel from './webappsmodel.js';
export * as flathubModel from './flathubmodel.js';
export * as flathub from './flathub.js';           // Flathub's jobs and state (the Store, Settings → Flatpak)
export { compareNames } from './collate.js';
import { windowIcon as windowIconOf } from './apps.js';
import { ImeOverlay } from './ime.js';
import { startOsk } from './osk.js';
import { startWindowMemory } from './windowmemory.js';
import { OskPanel, OskStatus } from './osk.jsx';
import { registerStatusItem } from './statusitems.js';
import { ClipboardPanel, ClipboardToast, clipboard as clipboardApi } from './clipboard.js';
import { SpotlightPanel } from './spotlight.js';
import { RemoteBadge } from './remote.js';
import { SharePrompt, sweepShareGrants } from './share.js';
import { CaptureCorner } from './capturestatus.jsx';
import { linuxIds } from './apps.js';
export { share, shareHolder, shareRev } from './share.js';
export { remote, remoteViewers, sshLogin, remoteCidr, remoteMinutesLeft, remoteAccess, remoteVideo, remoteIceServer, remoteFingerprint } from './remote.js';
export { inhibitShortcuts } from './kbinhibit.js';
export { shortcutsWho } from './kbinhibitmodel.js';
export { perms, permsRev, requestPermission, markDeviceUse, useIndicators, PermissionPrompt, registerPrivacySection, privacySections, PERMISSIONS, onPermissionRevoked, micStart, micStop, micLevel, shortcutsWhoText } from './privacy.js';
// the capture indicator by the clock (screen, camera, microphone): themes draw it as <PrivacyIndicators />
export { PrivacyIndicators, CaptureGlyph, useCaptureRows } from './capturestatus.jsx';
export { StudioPanel, StudioWindow, MicModePanel, studioAvailable, openStudioApp } from './studio.jsx';
export { captureRows, captureGlyph, captureSummary, rowStops } from './capturemodel.js';
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
import { RestorePrompt, startSessionRestore, markThemeSwitch, unmarkThemeSwitch, logOut } from './sessionrestore.jsx';
import { ThemeSwitchOverlay, showThemeSwitch, hideThemeSwitch, notifyThemeNotApplied } from './themeswitch.jsx';
import { askPower, canPower } from './power.jsx';
export { PowerDialog, powerAsk, POWER_SECONDS } from './power.jsx';
import { EmojiPanel } from './emoji.jsx';
export { admin } from './admin.js';
export { videoPrefs, setVideoPrefs, videoHardware, videoError, videoOptimizing, VIDEO_MODES, VIDEO_DEFAULTS } from './video.js';   // docs/VIDEO.md
import { startVideo } from './video.js';
export { browserFrameResult, browserFrameNotes, browserFrameState, browserTabsInTitlebar, setBrowserTabsInTitlebar } from './browserframe.js';   // docs/WAYLAND.md
import { startBrowserFrame } from './browserframe.js';
import { startNetStatus } from './netstatus.jsx';
import { startCleanerStatus } from './cleanerstatus.jsx';
import { startCloud } from './cloudstatus.jsx';
export { errorText, statusText, codeError, parseCode } from './errors.js';
export { logs } from './logs.js';
export { parseLordLog, sortFiles, fileGroup, fileLabel, isPrevious, isOlder, levelMatch, LEVEL_MAX, cloudAccounts, inRange, rowText, exportLine, SOURCE_GROUPS, toggleGroup } from './logfile.js';
export { LordOSLogo, LordOSGlyph, LordOSWordmark } from './logo.js';
export { Lumo, lumoSource } from './mascot.js';
export { DesktopWidgets, registerWidgetType, setWidgets, desktopWidgets } from './widgets.js';
export { BATTERY_DEFAULTS, batteryPrefs, batteryStep } from './batterypolicy.js';
export { useDevices, deviceList, deviceName, deviceShort, deviceValue, deviceVia } from './devices.js';
export { devicesFrom, fractionOf as deviceFraction, toneOf as deviceTone, kindOf as deviceKind } from './devicesmodel.js';
// (0.8.9.5) the apps used on each display: a theme's quick launch and its suggestion of the usual apps (sdk/displayapps.js)
export { startDisplayApps, useQuickApps, useDisplaySuggestion, quietDisplayApps, forgetDisplayApps, setDisplayAppsOn, displayAppsOn, displayAppsStored, openDisplayApps } from './displayapps.js';
export { registerWidgetKind, widgetKinds, widgetCatalog, addDeskWidget, removeDeskWidget, setDeskWidget, resizeDeskWidget, startDeskWidgets, deskWidgetSize, kindSizes, WidgetDropTarget, widgetDrag, deskWidgetsShown, widgetDisplayKey } from './deskwidgets.jsx';
export { Markdown, markdownHeadings, parseMarkdown, parseInline, toggleTask } from './markdown.jsx';
export {
  dock, useDockEntries, dockActivate, dockMenu, appWindowItems, registerDockMenu, registerDockActions, registerDockActivate, dockQuickActions, launcherIdOf, dockIdOf, dockIdOfWindow,
  launchDockItem, dockPrefs, DOCK_DEFAULTS, dockHideSettings, DOCK_HIDE_MODES, DOCK_SIZE_MIN, DOCK_SIZE_MAX, DOCK_MAGNIFY_MAX, dockBadgeMetrics, useDockCovered, quitApp, DOCK_LAUNCHER, useTrash, trashState, refreshTrash, openTrash, emptyTrash, trashMenu,
  dockDrag, dockPoof, beginDockDrag, registerDockTarget, dockDragJustEnded,
} from './dock.js';
export { cleanWindowLabel, WINDOW_LABEL_MAX } from './windowlistmodel.js';
// Chromium-family browsers' profiles (sdk/browserprofilemodel.js): read from their Local State as untrusted data
export { browserProfiles, refreshBrowserProfiles, windowProfile, noteLaunchedProfile, forgetLaunchedProfile, launchedProfiles } from './browserprofiles.js';
export { BROWSERS as PROFILE_BROWSERS, isProfileBrowser, profileChoice, cleanProfileChoices, cleanDisplayProfiles } from './browserprofilemodel.js';
export { sound, useSound, useSoundPeak, soundState, defaultSink, defaultSource, microphones, headsetMics } from './sound.js';
export { nearbyDevices, lowEnergySide, btErrorKey } from './btlistmodel.js';   // Settings → Bluetooth's nearby list (issue #78)
export { NO_SELECTION, clickSelect, keepShown } from './listmodel.js';   // a list's pointer selection (Shift range, Ctrl toggle), e.g. Backups' restore browser (#231)
export { SoundOutputList, useSoundOutputs } from './soundoutputs.js';   // choosing the output by the volume (issue #58)
export { monitor, useMonitor, monitorSamples, monitorProcesses, monitorConnections, monitorDevices, monitorDetail, MONITOR_HISTORY, diskTempHistory, DISK_TEMP_HISTORY, firewall, firewallState } from './monitor.js';
export * as fwRule from './fwrule.js';            // checks what the user types for a firewall rule
export * as netModel from './netmodel.js';        // (0.8.5) Monitor → Network: search tokens, filters, the tree
export { packages, packageList, refreshPackages, packageFile, loadWasm } from './packages.js';
import { refreshPackages, forgetGonePackaged, packageList as packageListOf } from './packages.js';
export {
  windows, wm, openedAt, useWindows, configureWindows, getApp, listApps, isLiveSystem, appIdOf, workArea, workAreaOf, pointerWorkArea, appMenus, useAppMenus, windowTitle, windowMenuTitle, windowAccent, appRegistry, appMenuClick,
  registerBuiltinApp, isSystemApp, snapPreview, snapRect, snapZoneAt, isWindowLocked, moveGrab, SNAP_DWELL_MS, locksChanged, snapEnabled, SNAP_ZONES, useWindowHidden, windowsOutOfSight, useWindowsOutOfSight, activeDesktop, desktopList, desktopNames, renameDesktop, moveDesktop,
  keepWindowsInWorkAreas, pointerDisplay, windowChrome,
} from './windows.js';
// Desktops and the Overview (docs/DESKTOPS.md)
export { desktopName, switchDesktop, nextDesktopBy, addDesktopAndShow, closeActiveDesktop, removeDesktopById, moveToDesktopItems, flowDesktopKeys, classicDesktopKeys, flowGrabs, ctrlArrowsOn, desktopSwipe, DesktopHud } from './desktops.js';
import { startDesktops, desktopsForThemeSwitch, desktopAt, indexOfDesktop } from './desktops.js';
// the brief level indicator of the volume, brightness and keyboard-light keys (docs/SHORTCUTS.md)
export { LevelHud, showLevel } from './levelhud.js';
export { overview, OverviewBackdrop, OverviewChrome, openOverview, closeOverview, toggleOverview, toggleAppWindows, overviewOpen, setDesktopDecor } from './overview.jsx';
export { codeStatus } from './codesig.js';
import { reportRefusedCode } from './codesig.js';
export { WindowShell, WINDOW_EDGE, useWindowButtons, titleBarPx, SeamCover } from './windowshell.jsx';
export { WINDOW_BUTTON_SIDES, windowButtonsPref, WINDOW_BUTTON_SIZES, windowButtonSizePref, buttonSizeScale, scaledBar, windowMenuTiling } from './windowbuttons.js';   // title bar buttons' side (issue #58), size (#151, #219), the Window menu's layouts (#151)
export { DRAG_MODIFIERS, dragModifierPref } from './moddragmodel.js';   // ❖ or Alt + drag anywhere in a window (#219)
import { startModDrag } from './moddrag.js';
export { term } from './terminal.js';
export { clipboard, clipboardHistory, clipboardPanel, clipboardKind, toast } from './clipboard.js';
export { restoreMode, setRestoreMode, restoreBrowsers, setRestoreBrowsers } from './sessionrestore.jsx';
export { spotlight, spotlightOpen, calc as spotlightCalc, convert as spotlightConvert } from './spotlight.js';
export { registerSpotlightSource, searchEverything } from './spotlight.js';
// session notifications, menu bar status items, Clock (alarms) and Help (docs/SYSTEM.md)
export { notify, dismissNotification, useNotifications, notifications, notifyHistory, clearNotifyHistory, NotificationBanner, setBannerPlacement } from './notify.js';
export { registerStatusItem, StatusItems, statusItems, setStatusPanelPlacer, statusPanelAt, statusPanelRoom } from './statusitems.js';
export { MoonGlyph, SunGlyph, ContrastGlyph, LayersGlyph, BluetoothGlyph, SignalGlyph } from './quickglyphs.jsx';   // quick settings' glyphs (issue #114)
export { setBacklight, useBacklight } from './hwkeys.js';   // the Control Center's brightness slider (issue #114)
export { MENUBAR_DEFAULTS, menuBarPrefs, menuBarPatch, cleanerItemShown, outputChoices, monthCells, monthWeeks, weekStartFor, wifiBars, wifiTile, wifiTileView, backlightOf, backlightValue } from './menubarmodel.js';   // what the bar shows (issue #58)
export { menuBarEveryDisplay, barDisplays, hasBar, displayIdFor, activeBarId, pinBar, focusDisplayId, panelLeft, inBar, insideDisplay } from './menubarmodel.js';   // a bar on every display
export { SUB_DELAY, SUB_GRACE, selectable, stepIndex, edgeIndex, menuAction, typeAhead, matchIndex, subPlace, aiming, barIndex, barHover } from './menunav.js';   // menus from the keyboard
export { sharedTicker, idleStore } from './ticker.js';
export { companion, statusItem, companionEvent, statusItemEvent } from './companion.js';
export { clockAlarms, clockTimer, timerStart, timerPause, timerResume, timerReset, timerLeft, stopwatch, swStart, swStop, swLap, swReset, swElapsed, nextAlarm, nextFire } from './alarms.js';
export { help } from './help.js';
import { NotificationBanner } from './notify.js';
import { startAlarms } from './alarms.js';
import { startDiskWatch } from './diskwatch.js';
import { startSentinela } from './sentinela.js';
import { maybeStartTour } from './help.js';
import { FirstStart, startFirstStart } from './firststart.jsx';
import { liveStartWindow } from './livestart.js';
import { getApp as getAppOf, isLiveSystem as isLiveSystemOf, wm as wmOf, windows as windowsStore } from './windows.js';
export const windowIcon = (w) => windowIconOf(w, getAppOf);
export { systemInfo, systemReport, lordosVersion, useSystemInfo, useServerStats, useCpuUsage, memUsedKB, formatBytes, formatDuration } from './system.js';
export * as fs from './fs.js';
export { useDirectory } from './fs.js';
export { ThemeProvider, useTheme, DEFAULT_TOKENS, solidOver } from './theme.js';
export {
  Button, Toggle, Spinner, usePending, afterPaint, useAppFocused, Segmented, SidebarItem, SectionTitle, ScrollView, TextInput, TextArea, FormFields, hasTextFocus, takeTextFocus, releaseTextFocus, textFocusOwner, Stepper, Slider,
  AppHost, useAppWindow, useKeyboard, focusToken, restoreFocus, pasteIntoFocused, FloatingWindows, useWindowState, useFilesDrop, FilesDropHint,
} from './components.jsx';
export { sortDrop, dropCount } from './filedrop.js';
// Keyboard focus (sdk/focus.js): a highlighted item that skips separators, for lists that open from a focusable control
export { menuStep } from './focusmodel.js';
export { slowClick, DOUBLE_CLICK_MS, SLOW_CLICK_MAX_MS, SLOW_CLICK_SLOP } from './slowclick.js';   // the slow second click that renames in place (Files)
import { FloatingWindows } from './components.jsx';
// One stacking order for the desktop: <Layer name>, <Overlay anchor> (docs/LAYERS.md)
export { LAYERS, Layer, Overlay, shellLayers } from './layers.js';
export { useNavigateButtons, useVisitHistory, navFor, visitNote, visitStep } from './navbuttons.js';   // a mouse's back and forward buttons (#234)
// where a small floating bar goes and comes back (the reading-aloud player's rule; Screenshots' toolbar uses it too)
export { placePlayer as placeFloatingBar, rememberPlace as rememberFloatingBar } from './voiceplayermodel.js';
import { Layer, enableShellLayers } from './layers.js';
export {
  ToolbarButton, ToolbarGroup, Toolbar, ToolbarSpacer, ToolbarDivider, Hairline, MenuButton, SearchField, EmptyState, GroupBox, FormRow, spacing, typeScale, controlSize, ConfirmDialog,
  Dropdown, Checkbox, RadioGroup, Tabs, Popover, Tooltip, ProgressBar, Disclosure, DisclosureGlyph,
} from './controls.jsx';
// (0.8.8.8) lists whose rows are chosen with the pointer and the keys: Shift ranges, Ctrl toggles, type-to-find
export { List, Table, TABLE_HEAD, tableWindow } from './lists.jsx';
export { media, useMedia, usePlayer, nowPlaying, useNowPlaying, linuxPlayers, useLinuxPlayers, NowPlayingTile, mediaFormatTime, thumbFor, useThumb, userDirs, openMarkup } from './media.js';
export { useCameras, refreshCameras } from './cameras.js';   // the camera list, made on a worker, again on hotplug
export * as imageEdit from './imageeditmodel.js';   // the image editor's math (crop presets, resize, redaction; issue #30)
export { disks, useDisks, needsDiskFormats, openDiskFormats } from './disks.js';
export { unmountedDisks, diskRelease, partMountable, partUnmountable } from './diskplaces.js';
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
export { vaultAvailable, vaultRequest, onVaultEvent, keyring, autofill, autofillReveal, KeyringPrompt, AutofillButton, askKeyringUnlock, passwordsState, saveLogin, SaveLoginOffer } from './vault.js';   // Passwords and keyring (docs/PASSWORDS.md)
export { loginHosts, shareHosts, typedSecret, loginEntry } from './loginmodel.js';   // sign-in forms and Passwords: exact hosts, typed or filled
import { KeyringPrompt } from './vault.js';
// Mail, Calendar, Contacts and Reminders through the sandboxed helper lord-maild (docs/MAIL.md)
// Databases: MySQL and MariaDB through the sandboxed helper lord-sql (docs/DATABASES.md)
export { sqlAvailable, sqlRequest, onSqlEvent, sqlFavorites, sqlPasswords } from './sql.js';
export { mailAvailable, mailRequest, onMailEvent, mailConfig, mailAccounts, mailStatus, autoconfig, OAUTH_PROVIDERS, DAV_PRESETS, davPreset, openDavSetup, openMailAccounts, trackMailStatus, isolatedPim, pickFileFd, closeFileFd } from './mail.js';
export { calendars, events as calendarEvents, reminders, usePim, usePimQuery, pimRev, startOfDay } from './calendar.js';
export { FilePicker } from './filepicker.jsx';
import { PortalFilePrompt } from './filepicker.jsx';
import { ScreenCastPrompt, ShotOverlay } from './screencast.jsx';
import { VoicePlayer, VoiceSavePrompt } from './voiceplayer.jsx';
import { LiveSpeechPanel } from './livespeech.jsx';
export { openFileWithApps, openUrl, revealPath, openFolder, openCalendarApp } from './sysopen.js';   // opening files/apps for the isolated system apps
export { books as addressBooks, contacts, displayName as contactName, initials as contactInitials, ContactPhoto } from './contacts.js';
import { startMail } from './mail.js';
import { startReminders } from './calendar.js';
// Settings' hardware pages (docs/SETTINGS.md): displays, Night Light, battery, Bluetooth, idle, clock
export {
  displays, useDisplays, applyDisplay, keepDisplay, revertDisplay, arrangeDisplay, setDisplayMirror, setMainDisplay, loginScreenScale, setLoginScreenScale, selfIsAdmin, setCaptureAllDisplays, setAutoRotateDisplays, displayConfirm, DisplayGlyph, nightPrefs, NIGHT_DEFAULTS, nightWindow,
  nightActive, nightAmount, setNightManual, previewNight, nightTick, sunTimes, battery, useBattery, BatteryGlyph, powerPrefs, POWER_DEFAULTS,
  powerProfiles, usePowerProfiles, onPowerProfileResult, bluetooth, useBluetooth, onBluetoothResult, setBluetoothPower, timedate, useTimedate, menuClock, clockPrefs,
  CLOCK_DEFAULTS, applyIdle,
} from './hardware.jsx';
import { DisplayConfirm, BluetoothPrompt, PolkitPrompt, PortalOpenPrompt, WebAppAskPrompt, WebAppInspectPrompt, WebAppRestartPrompt, WebAppMenu, startHardware } from './hardware.jsx';
import { GlobalShortcutsPrompt } from './globalshortcuts.jsx';
export { KeyComboField, useGlobalShortcuts, checkGlobalShortcut, setGlobalShortcut, forgetGlobalShortcuts } from './globalshortcuts.jsx';
import { startTray } from './trayitems.jsx';
// Settings → Displays: numbering, Identify (a big number on each real display) and magnetic arrangement
export { identifyDisplays, identifying as identifyingDisplays, IDENTIFY_MS } from './displaysui.jsx';
import { DisplayIdentify } from './displaysui.jsx';
export { displayNumbers, snapDisplay, resolveDrop, placementOk as displayPlacementOk, SNAP_PREVIEW_PX, scaleChoices, scaleTiles, loginScaleState } from './displaylayout.js';
// Fans and cooling (lord-fand, docs/FANS.md): Settings → Cooling and Monitor → Cooling
export { useFan, fanStatus, fanHistory, fanCase, fanControl, refreshFan, coolingVariant, heatColor, CoolingArt, GpuArt, FanCurveChart, formatTemp, FAN_HISTORY } from './cooling.jsx';
export { DEFAULT_CATEGORIES, defaultFor, defaultInfo, setDefaultFor, resetDefaultFor, candidatesFor, defaultOpenerId, nativeOf } from './defaults.js';
export { registerBundle, bundleFor } from './openwith.js';   // folders shown as one item (Machines' .lordvm)
export { registerOpenProvider, registerOpenProviders, openProvidersFor, openHintsFor, suggestedProviderFor, defaultProviderFor, setDefaultProvider, openExtOf } from './openwith.js';   // "Open with" providers (machines; later LordBridge, Wine)
// deactivated built-in apps (Store → Installed; docs/PACKAGES.md, "Deactivating LordOS apps")
export { appsOff, isAppOff, useAppsOff } from './appsoff.js';
export { DEACTIVATABLE, CORE_APPS, canDeactivate, isCoreApp, openersAvailable } from './appsoffmodel.js';
export { deactivateApp, activateApp, offerActivate } from './appsoffactions.js';
import { startAppsOff } from './appsoffactions.js';
// apps mode's guest side: inside a Machines guest, this desktop's windows go to the host one by one (guestwin.js)
export { guestAppsMode } from './guestwin.js';
export { guestLauncherItems, cleanGuestApps, sameGuestApps, launchAskStep, isGuestAppId } from './guestappsmodel.js';
import { startGuestWin } from './guestwin.js';
export { notifyPrefs, appNotifyPrefs, setAppNotifyPrefs, dndActive, NOTIFY_DEFAULTS } from './notify.js';

// ---- installed themes ----

export const themes = {
  list: () => lord.themes.list().map((t) => {
    let meta = {};
    try { meta = t.meta ? JSON.parse(t.meta) : {}; } catch (e) { /* invalid theme.json */ }
    // translated name and description: LordOS's own (shell.theme.<id>.*), then (0.8.9.6.2) the theme's own namespace
    // (theme-<id>.theme.name/description, a community theme's locales), then theme.json; a theme's texts go through
    // themeText (one line, no control or bidi-override characters, a fixed length)
    // (0.8.9) sketch, options and desktopArrows: what Settings shows for a theme it doesn't know by id (sdk/thememeta.js)
    const ns = themeNamespace(t.id);
    const own = (k, max) => themeText((ns && tOptional(`${ns}.theme.${k}`)) || meta[k], max);
    return { id: t.id, current: t.current, hidden: !!meta.hidden, name: tOptional(`shell.theme.${t.id}.name`) || own('name', THEME_TEXT_MAX.name) || t.id,
      description: tOptional(`shell.theme.${t.id}.description`) || own('description', THEME_TEXT_MAX.description), ...cleanThemeMeta(meta, t.id),
      // it couldn't start in this session (lord-server went back to the one before): Settings → Appearance says why
      failed: t.failed === true, failedWhy: typeof t.failedWhy === 'string' ? t.failedWhy : '', failedCode: t.failedCode === true };
  }).filter((t) => !t.hidden),         // the login screen (themes/greeter) can't be chosen
  current: () => lord.themes.list().find((t) => t.current)?.id || null,
  // the current theme's entry (null on the login screen or without lord.themes)
  info: () => { try { return themes.list().find((t) => t.current) || null; } catch (e) { return null; } },
  // Switches theme live, in the same session: lord-server checks the new theme's code first (the verified-code
  // check), then discards this JS context and loads the new one behind a short "Changing the desktop style" overlay
  // (themeswitch.jsx). The LordOS app windows that are open come back in the new theme (themeReopen, read once by
  // startShell); Linux app windows stay connected and simply get the new theme's frame. false: it can't be switched
  // to (refused or failed before): nothing changed, and a notification says so.
  use(id) {
    const before = settings.get();
    let reopen = [];
    try {
      reopen = windowsStore.get().filter((w) => { const a = getAppOf(w.id); return a && !a.builtin; }).slice(-8)
        .map((w) => ({ app: w.app, id: w.id, args: w.args ? JSON.parse(JSON.stringify(w.args)) : null, minimized: !!w.minimized,
          frame: { x: w.x, y: w.y, w: w.w, h: w.h }, maximized: !!w.maximized, desk: indexOfDesktop(w.desktop) }));
    } catch (e) { reopen = []; }
    let entry = null;
    try { entry = themes.list().find((x) => x.id === id) || null; } catch (e) { entry = null; }
    // (0.8.9.6.2) a theme whose theme.json asks for a newer LordOS (minLordOS) isn't loaded: nothing changes, and a
    // notification says which release it needs (Settings shows the same instead of Apply)
    let release = '';
    try { release = lordosVersionOf().version; } catch (e) { release = ''; }
    const needs = themeNeedsNewer(entry, release);
    if (needs) { notifyThemeNotApplied({ ...entry, needsLordOS: needs }); return false; }
    markThemeSwitch();
    settings.set({ theme: id, themeReopen: reopen.length ? reopen : null, themeDesktops: desktopsForThemeSwitch() });
    lord.settings.save(JSON.stringify(settings.get(), null, 2));
    showThemeSwitch(entry?.name || id);
    if (lord.themes.use(id)) return true;
    // not switched (lord-server answered before touching anything): the session goes on in this style
    hideThemeSwitch();
    settings.set({ theme: before.theme ?? null, themeReopen: before.themeReopen ?? null, themeDesktops: before.themeDesktops ?? null });
    unmarkThemeSwitch();
    lord.settings.save(JSON.stringify(settings.get(), null, 2));
    try { entry = themes.list().find((x) => x.id === id) || entry; } catch (e) { /* keep the one read before */ }
    notifyThemeNotApplied(entry || { id });
    return false;
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
  if (wanted && current && wanted !== current) {
    if (lord.themes.use(wanted)) return;
    // the chosen theme didn't start (it threw, or was refused at login) and lord-server kept this one: the choice
    // goes back to the theme that runs, and the person is told why the style didn't change
    let entry = null;
    try { entry = themes.list().find((x) => x.id === wanted) || null; } catch (e) { entry = null; }
    if (entry?.failed) {
      settings.set({ theme: current });
      notifyThemeNotApplied(entry);
    }
  }
  try { lord.input?.pointerAccent?.(null); } catch (e) { /* older server */ }   // LordOS's black arrow; a theme may tint it (setPointerAccent)
  refreshPackages();                    // after the theme's configureWindows
  startAppsOff();                       // deactivated apps open nothing; their background work stays off (appsoffactions.js)
  startGuestWin();                      // inside a Machines guest only: apps mode's endpoint (guestwin.js)
  enableShellLayers();                  // menus, popovers and prompts go to their layer (sdk/layers.js)
  resetShortcuts();                     // the dispatcher exists only in the session (sdk/shortcuts.js)
  startModDrag();                       // ❖ (or Alt) + drag anywhere in a window moves or resizes it (moddrag.js, #219)
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
  // (0.8.9) a new account's first session: choose the desktop (firststart.jsx); "Welcome to LordOS" waits for it
  if (!startFirstStart({ list: themes.list, current: themes.current, use: (id) => themes.use(id), onDone: maybeStartTour })) maybeStartTour();
  startHardware();                      // Night Light, idle timers, battery in the menu bar (hardware.jsx)
  startTray();                          // the tray icons of Linux apps next to the status items (trayitems.jsx)
  startDiskWatch();                     // notices when a disk gets too hot or its health gets worse
  startSentinela();                     // the process guard, once the person turns it on (docs/SENTINELA.md)
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
    at('modal', PrintDialog, RestorePrompt, FirstStart),
    at('notification', NotificationBanner, ClipboardToast, RemoteBadge, CaptureCorner, DisplayIdentify, VoicePlayer),
    at('system', PermissionPrompt, SharePrompt, Powerbox, KeyringPrompt, BluetoothPrompt, PolkitPrompt, PortalFilePrompt, PortalOpenPrompt, GlobalShortcutsPrompt, ScreenCastPrompt, ShotOverlay, WebAppAskPrompt, WebAppInspectPrompt, WebAppRestartPrompt, VoiceSavePrompt, DisplayConfirm, WebAppMenu),
    at('input', OskPanel, ImeOverlay, ThemeSwitchOverlay));   // the style changing: over everything (themeswitch.jsx)
}
// the wallpaper slideshow (0.8.9; a theme turns it on in theme.json: sdk/thememeta.js)
export { SlideshowWallpapers, withDownloadedStills, slideSrc, slideshowPrefs, slideshowPatch, slideshowStop, slideshowCategories, SLIDESHOW_EVERY } from './slideshow.jsx';
export { setLoginStill, setLoginImage, addWallpaperImage, removeWallpaperImage, MotionWallpaper, MOTION_SECTIONS, MOTION_RATES, motionBusy, motionItems, motionThumb, motionPoster, motionDownloads, useCodecs, downloadMotion, removeMotion, fetchCatalogFile, removeCatalogFile, setLoginMotion, variantFor } from './motion.jsx';
export { userVideoProblem, motionCodecs, MAX_SECONDS } from './motionmodel.js';
export { lordosWallpapers, userWallpaperName, wallpaperSrc, wallpaperThumb, findWallpaper, wallpaperIdFor, wallpaperPatch, displayWallpaperKey, displayLabel, DEFAULT_WALLPAPER, GREETER_WALLPAPER, WALLPAPER_CATEGORIES, STILL_STORE, stillItems, twinItems, withTwinFile, stillThumb, stillStore, stillFiles, refreshStillFiles, downloadedStills } from './wallpapers.js';
export { appGroup, appMenuItems, runningApps, forceQuit, recentItems, storeUpdates, hideApp, hideOthers, toggleShowDesktop, appSettingsItem, webHelpItems, webAppearanceItem, webUserAgentItem } from './appcontrol.js';
export { windowMenuItems, moveResizeItems, registerWindowMenuItems, alwaysOnTopItem } from './windowmenu.js';
export { setDesktopCount } from './desktops.js';
// Apps in a small window (a Workspaces tile): the window's width, a breakpoint and a sidebar that hides (sdk/narrow.jsx)
export { useWindowWidth, useWindowHeight, useNarrow, useSidebar, SidebarToggle, SidebarOverlay, NARROW, MIN_W, MIN_H } from './narrow.jsx';
// Network streams: cameras, IPTV and the Player's addresses through lord-stream (sdk/streams.jsx, sdk/streammodel.js)
export { useStream, saveStreamPassword, STREAM_KEYRING_APP } from './streams.jsx';
export { streamUrlOk, splitCredentials, withCredentials, protocolOf, lowQuality, parseM3U, hostKey } from './streammodel.js';
export { VideoSetup, videoSetup } from './videowidget.jsx';

export { WidgetPicker, widgetPicker } from './widgetpicker.jsx';
// (0.8.9) the first-start desktop chooser and Desktop style's drawn pictures (Settings → Appearance)
export { FirstStart, startFirstStart, firstStartState, useFirstStartOpen } from './firststart.jsx';
export { DesktopStyleThumb, SketchThumb, knownStyleTheme } from './stylethumb.jsx';   // Add widget, in every theme (0.8.8.10)

// A theme's own pointer (0.8.9, Lord Workspaces): the default arrow as a rounded tip in its accent colour ('#RRGGBB'),
// or null for LordOS's black arrow. Only the default pointer changes. startShell puts the black one back for every theme.
export const setPointerAccent = (color) => { try { return !!lord.input?.pointerAccent?.(color || null); } catch (e) { return false; } };
