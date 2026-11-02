# Minimal: an example LordOS theme

A deliberately small theme that shows every piece a LordOS theme needs, for the guide
[`docs/THEMES-GUIDE.md`](../../docs/THEMES-GUIDE.md):

- `theme.json`: the name and description Settings → Appearance → Desktop style shows;
- `tokens.js`: the base tokens every app reads (`mode`, `accent`, `text`, `textDim`, `windowBg`,
  `sidebarBg`, `separator`, `hover`, `control`, `inputBg`), the `accents` map, the wallpapers, and a
  few keys of its own (`titleBg`, `windowBorder`, `barBg`, `blur`, `shadow`);
- `index.jsx`: the work area (`configureWindows`), a window frame built on `<WindowShell>` and
  `<AppHost>` (moving and resizing in C), Linux windows that draw their own frame (`args.csd`) and
  their window menu, a bar in the `dock` layer with an apps menu, one button per window and a clock,
  menus in the `popover` layer built from the SDK's item lists (`useAppMenus`, `windowMenuItems`),
  the power confirmations, desktop widgets, a wallpaper per display, and `startShell`.

It has no text of its own: every label is an existing key of the `shell` catalog
(`shell.menu.apps`, `shell.menu.settings`, `shell.menu.logout`, `shell.menu.window`,
`shell.classic.minimize`, `shell.classic.maximize`, `shell.classic.restore`, `shell.classic.close`),
so it is translated into every language LordOS ships. A theme with new text adds its own catalog (see
the guide, "Translations").

## Building it

Themes are built from the `themes/` folder of a LordOS development tree; `tools/build-theme.mjs`
has no option for a theme elsewhere. Copy this folder in and build only it:

```sh
cp -R examples/theme-minimal themes/minimal
node tools/build-theme.mjs minimal     # → themes/minimal/theme.bundle.js (about 1.8 MB)
```

The build also rewrites `code-manifest.json` with the new bundle in it (and removes a
`code-manifest.sig` that no longer matches). That is what lets a development build of lord-server
load it; a release build will not (see the guide, "Verified code: what happens to a theme that
LordOS did not sign").

`themes/minimal` is a copy for your own tree: don't commit it to LordOS.

## Status

Built with the repository's own tooling (`node tools/build-theme.mjs minimal`, esbuild 0.24.2) on
2026-10-26 in a copy of the tree, where `node tools/i18n-check.mjs` also passed with it in `themes/`.
**Not yet run on a LordOS session**: there is no screenshot of it,
so treat it as a starting point and check it with the guide's checklist.
