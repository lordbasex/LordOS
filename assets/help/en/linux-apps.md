---
title: Linux apps (GNOME and KDE)
section: desktop
order: 15
keywords: wayland, gtk, qt, firefox, linux apps, debian
---
# Linux apps (GNOME and KDE)

LordOS has its own Wayland compositor, so Linux apps —GTK (GNOME), Qt (KDE), Firefox, Electron—
open as regular desktop windows.

- **Install them:** from the [Store](help:store), which lists Debian apps with their icons and
  descriptions.
- **Open them:** Apps, the Dock or Search, like any app.
- **Open a file with one of them:** in [Files](help:files), right-click → “Open with”.
- **Clipboard:** what you copy in a Linux app pastes in LordOS and the other way round, images too.
- **Keyboard and touchpad:** they get the keys (❖ combinations still belong to LordOS, except in Lord Flow, where [❖C, ❖V and others arrive as Ctrl](help:shortcuts)) and the
  pinch and swipe gestures.

Apps that draw their own frame (for example GNOME apps with libadwaita) show without the theme's
frame. For safety, LordOS only opens installed apps (their `.desktop` files), never a typed command.

## Frame and colors

- **Chromium and Firefox** use the LordOS frame (the theme's traffic lights). To go back to the
  browser's own tab strip: in Chromium, ⋮ menu → Settings → Appearance → turn off "Use system title
  bar and borders"; in Firefox, ☰ menu → More tools → Customize toolbar → uncheck "Title Bar".
- **Brave** (and other browsers built on Chromium) draws its own bar. With its "Classic" theme the
  window buttons are its own, not LordOS's. Settings → Appearance → "Browsers: tabs in the title
  bar" also changes Brave's profiles (quit Brave first, or it applies when Brave closes); in Brave
  itself: ☰ menu → Settings → Appearance → Theme → GTK. To use the LordOS frame instead: turn on
  "Use system title bar and borders" there.
- **GTK and GNOME** follow LordOS's dark or light mode and accent color live, and put the window
  buttons on the left, like the traffic lights (on the right with the Lord Classic theme). GTK 3 apps use
  the "LordOS" theme; their accent color is fixed.
