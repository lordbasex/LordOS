## In a few words

Since **LordOS 0.8.9.4**, the thousands of Linux apps on **Flathub** install on LordOS, and **everything is done from the Store**. You turn Flathub on there, browse its apps, see what each one asks for and what LordOS does about it, and install, update or remove it, without a terminal. **Settings → Flatpak** shows how much space the apps take, and removes Flatpak completely if you change your mind.

What makes it LordOS's: every Flathub app runs under **LordOS's own rules**, not only its own. It gets its own sandbox inside LordOS's own view, your **Firewall** and your restrictions apply to it like to any app, and your **home folder is read-only** for it except for its own folder and your personal folders.

[[shot:0]]

It works on **64-bit PCs** (x86-64) today; the Raspberry Pi will come later.

## Everything from the Store

**Turning it on.** Store → **Flathub** → **Turn on Flathub**. The Store installs `flatpak` from Debian like any other package (6 packages, 11 MB, nothing installed until you press the button), adds Flathub with the key LordOS ships, and fetches the catalog.

**Browsing.** Flathub's apps show in their own section, in Discover and in search, with Flathub's own names, icons and categories. By default the Store shows **only the apps Flathub verified as published by their own developers**; a switch shows the rest, which are marked "Not verified".

**The shop first, then what you installed** (in development; the latest release is 0.8.9.5). The Flathub section opens on its shop: the home, the collections and the categories. Its second column starts with **Installed**, with how many Flathub apps you have, and the categories below it, the way the Store's own sidebar has Installed among its pages. Installed lists only your Flathub apps, with Update Flathub apps, Remove and Open. Store → **Installed** lists every app you have, whatever it came from, with a filter for each source (LordOS, Debian, Flathub, web apps).

**A clean shop, with Settings next to Installed** (in development; the latest release is 0.8.9.5). The section is called **Flathub apps** and opens straight on its banner. Only verified apps, Flathub's rankings, Refresh and Turn off are in its **Settings**, right under Installed in the second column; a small ↻ icon next to the search refreshes the catalog, and the search field ("Search Flathub apps") searches only Flathub. Discover's search still looks everywhere.

**Popular, Trending and the app of the day** (since 0.8.9.5). Flathub's home in the Store opens on Flathub's own app of the day and rows of Apps of the week, Popular, Trending, Recently added and Recently updated. LordOS asks `flathub.org` for those lists at most once a day, with a fixed request that says nothing about you, and keeps only the apps' ids: names, icons and descriptions still come from Flathub's signed catalog, and the install rules don't change. A switch in the Store turns it off; without internet the rows simply aren't there.

**Before installing**, each app's page says who publishes it, its license, and **what it declares**: internet, sound, your files, devices, other apps on the bus. Next to each one, what LordOS does about it: what you can block in the Firewall, what LordOS limits, and what it never grants.

[[shot:1]]

[[shot:2]]

**Installing, updating, removing.** One button each, with the progress on the page. Removing an app also removes the runtimes no other app uses any more.

**Chromium and Electron apps** (since 0.8.9.5): they install like any other. In 0.8.9.4 they were marked "not yet supported"; now the sub-sandboxes their helpers need are served inside the app's own sandbox.

## Settings → Flatpak

Settings has a **Flatpak** page of its own: whether Flatpak is installed, Flathub shown in the Store or not, how many Flathub apps there are and how much they, their runtimes and their own data take, and a button to open Flathub in the Store.

**Remove Flatpak completely** takes away every Flathub app, the runtimes they used, Flathub itself and the `flatpak` package, with the packages listed before anything happens and the administrator password asked, and optionally moves the apps' own data to the Trash. Afterwards nothing of Flatpak is left on the computer.

[[shot:3]]

[[shot:4]]

## In your desktop, like any app

Flathub apps show in search, in the launcher and in Files' **Open With**, and you can keep them in the **Dock**. They get LordOS's window frame, its menu bar and its notifications with the app's own name and icon.

[[shot:5]]

[[shot:6]]

When you open a file with a Flathub app, **it gets that one file**, nothing else of your home.

[[shot:7]]

GTK 4 and GTK 3, Qt and KDE, X11-only apps and Firefox's own sandbox all work:

[[shot:8]]

[[shot:9]]

[[shot:10]]

## Under your Firewall and your rules

Each Flathub app runs in **its own root-owned scope** (`flatpak:<id>` in the Firewall and in Monitor), so nothing it starts can leave it. That gives it everything Linux apps have:

- **No internet, LAN only, per-domain rules, blocklists**, applied by the kernel, with its own row in Monitor → Network.
- **No camera, no microphone, no USB, no Bluetooth**, and **no access to your files**, in Settings → Privacy.
- **No notifications**, if you want quiet.

Flathub's own downloads (the catalog, installs and updates) are a row of their own too, **Flathub**, which you can see and block.

[[shot:11]]

[[shot:12]]

## What LordOS never grants

Whatever an app declares, or an override file asks for later:

- **Your home folder is read-only** for it, except its own folder (`~/.var/app/<id>`) and your personal folders (Documents, Downloads, Pictures…). No startup file, key or autostart entry can be written, created or renamed into place.
- **LordOS's own folders are hidden**: your settings, Firewall rules, permissions, launchers and autostart.
- **The session bus only through Flatpak's filtering proxy**, and never the names that run commands outside the sandbox (`org.freedesktop.Flatpak`, systemd) or hold your saved passwords (the Secret Service), nor any wildcard that would cover them.
- **No system bus**, and never LordOS's own services to impersonate.
- **No X11** for an app you restricted (X11 apps can watch each other).

## The decisions behind it

| Decision | Why |
|---|---|
| Every app in LordOS's own view and scope | So your Firewall, your restrictions and Monitor see the app as itself. A plain `flatpak run` would move itself out of any scope LordOS gives it. |
| Read-only home, whatever the app declares | A startup file or autostart entry written by an app runs outside every sandbox at your next login. |
| Verified apps by default | Flathub checks that a verified app comes from its own developer. The rest are one switch away, marked. |
| Installed per user, never system-wide | No root, no polkit: Flatpak's root helper is masked. |
| Flatpak's own portal and session helper masked | What they start runs outside the app's sandbox. LordOS serves the sub-sandboxes GNOME apps need itself, inside the same app's sandbox. |
| The catalog read in a sandbox of its own | Flathub's catalog is parsed by a small helper with no network and no files, never by the desktop itself. |
| Its own Firewall row for Flathub | Downloads are visible, and you can stop them like any app. |
| Chromium and Electron helpers in the app's own sandbox | Their sub-sandboxes are started by a small helper inside the app's view, with the app's own limits and no more (since 0.8.9.5). |
| Nothing in the background | No Flatpak service runs unless an app is open; LordOS's own part exists only while `flatpak` is installed. |
| x86-64 first | Tested there; the Raspberry Pi comes after. |

## What was tested

On a test machine on 20 November 2026, an **automated test plan** of 31 checks runs every time (`server/tests/fwflatpak/flatpak.sh`): Flathub's Firewall row, each app's identity, what is never granted even with an override, no files, no internet, the sub-sandboxes, X11 with and without a restriction, and the microphone restriction between two apps. The apps tried by hand: **GNOME Text Editor, Decibels, Shortwave, Meld, KCalc, Firefox and GNU Emacs**. A security review of the whole change found four problems, all fixed before the release.

**Since 0.8.9.5:** Chromium- and Electron-based apps, checked on 22 November 2026 with Whatsie (Qt WebEngine), Chromium and FreeTube (Electron); DRM video (Widevine) isn't available to them. **Not yet:** Spotify starts only with its own Wayland flags (its developer's choice, not LordOS's); the Raspberry Pi.

The full design, every decision and every test result are in `docs/FLATPAK.md`.
