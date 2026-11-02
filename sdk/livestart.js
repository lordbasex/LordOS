// livestart.js — what the live ISO opens by itself when the desktop starts. No imports, so
// tests/livestart_test.mjs can load it.
//
//   liveStartWindow(info, hasApp)   the window to open: [id, options], or null for none
//     info    lord.system.info(): `firstAccount`, `live` and `liveCheck` (the boot entry "Check this computer",
//             lordos.check=1 on the kernel command line, docs/ISO.md)
//     hasApp  (id) => whether that app exists
//
// The OVA image's first boot (`firstAccount`, docs/ISO.md) opens the installer on its account screens.
// The installer normally; System Information on its Performance tab for that boot entry, without the
// installer in front (it stays in the launcher). Nothing here starts a measurement.

export function liveStartWindow(info, hasApp) {
  if (info && info.firstAccount) return hasApp('installer') ? ['installer'] : null;
  if (!info || !info.live) return null;
  if (info.liveCheck && hasApp('sysinfo')) return ['sysinfo', { args: { tab: 'perf' } }];
  return hasApp('installer') ? ['installer'] : null;
}
