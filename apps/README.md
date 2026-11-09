# LordOS apps

Each app is a folder `apps/<id>/` with two files. Apps don't depend on the theme: any theme shows
them with its own colors (via `useTheme()`).

```
apps/quicknotes/
├── manifest.json
└── index.jsx
```

**manifest.json**

```json
{
  "id": "quicknotes",
  "version": "1.0.0",
  "name": "Quick Notes",
  "description": "Quick notes",
  "size": [500, 400],
  "dock": true,
  "permissions": []
}
```

`version` is required: the app's own version, in semver (`major.minor.patch`, optionally with a
`-beta.1` or `~rc1` suffix). The build refuses an app without one (`tools/build-theme.mjs`), and the
Store shows it next to the app (Installed: "LordOS app · 0.8.5"). The native apps started at `0.8.5`,
the LordOS version they first shipped in; from then on an app's version changes when the app changes,
apart from LordOS's, so the apps can later live in repositories of their own. An app added later starts
at the release it first ships in (`cleaner` and `remote-desk`, 0.8.7). A release package refuses an app whose files
changed since the previous release while its version stayed the same (`node tools/app-versions.mjs`, run by
`tools/iso/mkdeb.sh` for a release; `--bump` raises the patch number of each one).

Optional fields: `"transparent": true` (the theme paints no background under the content; the
app draws its own, e.g. the Terminal with transparency) and `"minSize": [width, height]`.

**index.jsx**

```jsx
import { View, Text, useTheme, TextInput } from '@lordos/sdk';
import { useState } from 'react';

export function Icon({ size }) {          // icon for the dock, app launcher, taskbar
  return <View style={{ width: size, height: size, borderRadius: size * 0.22, backgroundColor: '#ffd60a' }} />;
}

export default function QuickNotes() {    // the window's content
  const t = useTheme();
  const [text, setText] = useState('');
  return (
    <View style={{ flex: 1, padding: 16, gap: 10 }}>
      <Text style={{ fontSize: 18, fontWeight: 700, color: t.text }}>Quick Notes</Text>
      <TextInput value={text} onChange={setText} placeholder="Write…" />
    </View>
  );
}
```

(Real apps don't hard-code visible text like this: see "Texts and languages" below.)

`npm run build` rebundles every theme with every app. To distribute an app without touching the
themes, it is packaged as an `.lpk` with `node tools/lpk.mjs <folder>` and installed from the
Store (see `docs/NATIVE-APPS.md`).

## Available API (`@lordos/sdk`)

| Area | API |
|---|---|
| UI | `View`, `Text` (with `fontFamily: 'monospace'` for JetBrains Mono), `Image`, `ScrollView`, `TextInput` (with `onKeyDown`), `Button`, `Toggle`, `Segmented`, `Stepper`, `Slider`, `SidebarItem`, `SectionTitle`, `Terminal` |
| Theme | `useTheme()` → `text`, `textDim`, `accent`, `windowBg`, `sidebarBg`, `separator`, `hover`, `control`, `inputBg`, `mode` |
| Windows | `wm.open(app, { new, instance, frame, args })`, `wm.close(id)`, `wm.focus(id)`, `wm.setTitle(id, t)`, `wm.setMenus(id, menus)`, `wm.setCloseGuard(id, fn)`, `useAppWindow()` → `{ id, focused, args }` |
| Terminal | `term.create/close/configure/key/paste/search/...` (see `docs/TERMINAL.md`), `clipboard.get/set`, `clipboardHistory` |
| Keyboard | `useKeyboard(fn)` (only with the window in front), `TextInput`; `keyboard.list/configured/active/next/set` (XKB layouts) |
| Language | `t('namespace.key', vars)`, `useLocale()`, `appName(app)`, `formatDate`, `formatTime`, `formatNumber`, `languages()`, `setLanguage(id)`, `direction()`, `textDirection(text)` — see `docs/LANGUAGES.md` |
| Input methods | `TextInput` already uses them; for a custom field, `ime.attach(token, caret)` / `ime.detach(token)` and `useIme()` (the on-screen keyboard also learns about the field and its caret from these) |
| Administration | `admin.run('locale-gen' \| 'system-locale' \| 'timezone' \| 'langpack', arg, onLog)` → promise; `admin.busy` (progress) |
| Files | `useDirectory(path)`, `fs.list`, `fs.read(path, { binary })` (max. 8 MB), `fs.write(path, text \| ArrayBuffer)` (atomic, max. 32 MB), `fs.mkdir`, `fs.rename`, `fs.copy`/`fs.move` (in the background: `fs.jobs()`), `fs.trash`/`trashList`/`restore`/`removeFromTrash`/`emptyTrash` (freedesktop Trash), `fs.stat`, `fs.du`, `fs.free`, `fs.search`, `fs.recent`, `fs.volumes`, `fs.join`, `fs.parent`, `fs.basename` — only in the home folder and on the user's mounted disks (`/media/$USER`, `/run/media/$USER`); writing to `~/.local/share/applications`, `~/.config/autostart`, `~/.local/share/lordos/apps` or the settings is not allowed, and deleting is only possible inside the Trash (see `docs/FILES.md`) |
| Fonts | `lord.fonts.list({ rescan })` → `{ family, style, ps, weight, italic, mono, color }` of the installed fonts |
| Touchpad | `onWheel` with `e.precise`, `e.deltaX/deltaY` in pixels and `e.phase` (`'momentum'` = inertia); `onPinch({ scale, rotation, x, y, phase })`; `onSwipe({ dx, dy, fingers, phase, system })`; `useTrackpads()`, `trackpad.config()/set(changes)`, `registerGestureActions({ up, down, left, right })` (themes) — see `docs/TRACKPAD.md` |
| System | `useSystemInfo()`, `useCpuUsage()`, `useServerStats()`, `formatBytes`, `formatDuration` |
| Settings | `useSettings()` → `[settings, update]`, stored in `~/.config/lordos/settings.json` |
| Session | `session.quit()`, `themes.list()`, `themes.use(id)` |
| Apps | `useLauncherApps()` (LordOS, packages and Linux apps), `launchApp(id)`, `packages.*`, `windowIcon(w)` — see `docs/NATIVE-APPS.md` |
| Clipboard | `clipboard.get/set(text, { secure })/use/pin/remove/clear`, `clipboardHistory` and `clipboardKind` (saved history; ❖V and ❖⇧V panels) — see `docs/SPOTLIGHT.md` |
| Search | `spotlight.open/close/toggle` (❖Space; `open(display)` and `toggle(display)` open it on that display); `spotlightCalc(text)` and `spotlightConvert(text)` |
| Sound | `useSound()`, `sound.setVolume/setMute/setDefault/setPort/move/meter/play`, `defaultSink(s)`, `microphones(s)` — see `docs/SOUND.md` |
| WebAssembly | the standard `WebAssembly` global (`docs/WASM.md`); in installed apps, `loadWasm(id, path)` |
| Canvas | `<Image pixels={{ width, height, data }} />`: RGBA pixels (for example, computed in WASM) |
| Media | `media.play/image/thumb/probe/poster/edit/transcode/screenshot/annotate/saveImage/copyImage/screenRecord/audioRecord/camera/photo/cameraRecord`, `usePlayer(id)`, `<MediaView media={id} />`, `nowPlaying`, `NowPlayingTile`, `useThumb(path)`, `userDirs()` — decoded by `lord-media`, sandboxed (see `docs/MEDIA.md`) |
| Windows (media) | `wm.setProtected(id, on)` (black in screenshots, recordings and remote desktop), `wm.setFullscreen(id, on)`, `wm.setPip(id, frame)`, `useWindowState(key, initial)`, `useAppWindow().mode` (`'window'`, `'fullscreen'` or `'pip'`) |
| Markdown | `<Markdown source base width onLink onToggleTask />`, `markdownHeadings(text)`, `toggleTask` — see `docs/NOTES.md` |
| Desktop | `registerWidgetType(type, Component)`, `setWidgets(type, [{ id, x, y, w, h, pinned }])`: widgets between the wallpaper and the windows |
| Notifications (M9) | `notify({ app, title, body, actions: [{ id, label, primary, run }], sound, sticky, timeout, onClose })` → id, `dismissNotification(id)`, `useNotifications()`, `notifyHistory`; `registerStatusItem(id, Component)` (status icons in the theme's bar) — see `docs/SYSTEM.md` |
| Packaged apps | `companion.on(fn)`, `companion.send(value)`; `statusItem.set({ icon, title, alert, menu })`, `.clear()`, `.onSelect(fn)` for any app with `status-item` — see `docs/PACKAGED-APPS.md` |
| Clock and Help (M9) | `clockAlarms`, `clockTimer`/`timerStart…`, `stopwatch`/`swStart…` (keep running with the app closed); `lord.time.zoneOffset(zone, t)`; `help.list/read/search/open/startTour`; `registerSpotlightSource(id, fn)`; `weekdayName(d)` |
| Documents (M9) | `pdf.open(path \| { blob })` (page, search, outline, links, forms, annotations, save a copy), `<DocImage image>`, `archive.list/extract/create`, `fontInfo/fontSample/fontGrid`, `pdfThumb`, `makeTextPdf`/`makeImagePdf`: all in the sandboxed helper `lord-doc` — see `docs/DOCUMENTS.md` |
| Printing (M9) | `print.open({ title, path \| doc \| text \| image })` opens the system dialog; `print.printers/jobs/cancel/add/remove/setDefault/discover/scanCaps/scan` — see `docs/PRINTING.md` |
| Emoji (M9) | `emoji.open()` (❖.), `emoji.search/name/withTone/insert`, `EmojiGrid`, `EmojiToneBar` |
| Custom text focus | `takeTextFocus(token, onKey, onChange)` / `releaseTextFocus(token)`: a custom field receives keys like a TextInput |

## Texts and languages

Visible texts aren't written in the code: they go through `t('myapp.key')` with their
translations in `assets/locales/{es,en,pt}/myapp.json` (`{ "key": "Text" }`). The app's name is
translated in `apps.json` (`"myapp": "My app"`). Lists containing texts are built at render
time, not as module constants, because the UI is remounted when the language changes.
`node tools/i18n-check.mjs` warns if any key is missing.

## Permissions

For an app built into the theme, `permissions` in the manifest is **declarative**: it documents what
the app uses, and the app runs in the same JS context as the theme. Apps with `"process": "isolated"`
and installed `.lpk` apps run as their own process, with permissions checked by the server
(`docs/NATIVE-APPS.md`, `docs/PRIVACY.md`). For all of them the server runs without root and there is
no API to run commands (the terminal can only open the user's shell).

`fs` only writes inside the home folder and the user's mounts and never to the folders the system uses to launch or
install things (`.desktop`, autostart, installed apps), so an app can't turn a write into
running a command.
