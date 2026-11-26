# Many appearances: community themes, installing from a file, and a theme store (plan)

Status: **plan, asked for by the maintainer on 2026-11-21**. Stage 2 is built (0.8.9.6.2, in development); stages 1 and 3 as noted. How a theme is made today:
[THEMES-GUIDE.md](THEMES-GUIDE.md). Steps that widen what third-party code can do go through the maintainer's review
and an AGENTS.md entry before they are built.

## The starting point

A LordOS theme is privileged code: a React bundle that lord-server runs in the shell's QuickJS context, with the whole
`lord.*` API (files, terminal, network, admin requests, every key and pointer event). It carries its own copy of the
SDK, so it also controls what permission prompts show, and it can draw a lock screen or a password prompt. Release
builds run only the bundles listed in the signed code manifest (`codesig_load`). So a theme from a user is as
sensitive as installing a program with full access — validation can't stop at a well-formed manifest.

The first community theme was reviewed on 2026-11-21 ("Caelestia", a layout inspired by caelestia-dots/shell; findings
below). It is a useful test case for every stage of this plan.

## Stage 1 — Settings → Appearance for many themes

- A gallery instead of one row: search, filters (Official / Community / Installed), a large preview, the author, the
  version, and an **"Unofficial"** badge for anything LordOS didn't make.
- Each theme's options (`theme.json` `options`) open from its card.
- A theme that failed to load shows "Couldn't start — back on ‹previous›" with Report (it opens an issue form with the
  log, never sends anything by itself).

## Stage 2 — install a theme from a file

**Built (0.8.9.6.2, in development; approved by the maintainer on 2026-11-26):** [THEMES-GUIDE.md](THEMES-GUIDE.md#6-community-themes-installed-from-a-file).
What differs from the plan below: capabilities give read access only (no setters); `notifications` is a capability
(a notification centre's history); a community theme may launch apps through the shell's list.

- **Browse… → a `.zip`** with a `theme.json` manifest: id, name, author, license, version, minimum LordOS/SDK version,
  the capabilities it needs (below), its options, and the source files.
- **Validation on install:** schema; allowed file types and sizes; no path traversal or links; **source only** (LordOS
  builds the bundle itself, never runs a prebuilt one); static checks (only `@lordos/sdk` and `react` imports; no
  `eval`/`Function`/dynamic `import()`/remote URLs/`globalThis`/obfuscation); every visible string through `t()`.
- **Trust:** an installed community theme is unsigned, so it runs only after the person confirms once ("This theme
  isn't from LordOS. It can see what you type while it's on screen…"), shown with the author.
- **Restricted surface (needed before stage 2 ships):** the system layer — permission prompts, notification banners
  and their actions, privacy indicators, polkit/keyring prompts and the lock screen — moves out of the theme's reach so
  a theme can't replace or hide it; theme code gets a capability-filtered `lord.*` (no terminal, file writes, admin,
  permissions or direct app launching — `launchApp` goes through the shell). The manifest declares capabilities such as
  `wifi`, `bluetooth`, `brightness`, `media` and LordOS grants only those, as for isolated apps.
- **Watchdog with fallback (also useful for official themes):** an exception at start, no first frame within a few
  seconds, a long JS stall or repeated crashes → back to the previous theme (or Lord Flow), the theme marked failed, the
  error kept for a report; plus a fixed key combination that always restores the default theme.

## Stage 3 — a theme store ("Desktop styles")

- A catalog like the VS Code Marketplace: screenshots, author, ratings, downloads, versions, "Install".
- **Publishing:** an author submits the source (an issue or a form on lordos.org). LordOS reviews it (the checks of
  stage 2 plus a human review), builds it reproducibly, and signs it with a store key (a sub-key, separate from the
  release key) in a second signed manifest. Signed store themes show **"Verified"**; they still run with the
  restricted surface.
- Updates come the same way; a theme that stops passing review is pulled from the catalog and turned off with a notice.
- Official themes stay in the release.

## Review of the first community theme ("Caelestia", 2026-11-21)

- **Safe as code:** no eval, remote URLs, obfuscation, terminal, file or app-launch calls; only Wi-Fi/Bluetooth/
  brightness/media APIs and existing admin actions that Settings already uses; key listeners only for Escape/arrows.
- **Not shippable yet:** it dismisses every system notification banner (stops a ringing alarm, hides the "component
  modified" warning, loses actions); no privacy indicators or status items; shutdown/restart/log-out without
  confirmation; ~80 hard-coded English strings; no app menus or running-apps list; no right-to-left; dark only; settings
  outside `theme.json` options; leftovers from other prototypes ("Serpantinum", "Rose HUD") and 33 unused PNGs.
- **Provenance:** no author or license in it. The code is new JSX (the original project is QML, GPL-3.0), so only the
  layout idea is taken; shipping it under MIT needs the author's written statement and the origin of the other
  prototypes' parts and icons. Credit "inspired by Caelestia (caelestia-dots/shell)", and consider a name of its own.
- **Fixes:** about 3–5 days of work at human pace (hours with agents): the eight items in the review (notifications,
  indicators, confirmations, i18n, app menus/dock, RTL and light mode, options/cleanup/attribution, per-display).
