// pre2.js — goes after tests/machines_sheets_scene/pre.js and before the theme bundle in tests/machines_see_headless.sh (a
// test tool, never shipped): the same made-up library, with "Ubuntu 24.04" given a chosen kernel for Start Linux directly
// (kernelFile, initrdFile, kernelCmdline, kernelBoot) and channels on (clipboard both ways, the transfer folder, a shared
// folder, guest files and notifications), "Base Debian" with Secure Boot (so its Start Linux directly is refused), and a
// computer that misses passt (so the assistant's Review offers What's Missing).
(function () {
  const list = lord.vm.list;
  lord.vm.list = () => list().map((m) => (m.name === 'Ubuntu 24.04'
    ? { ...m, kernelBoot: true, kernelFile: true, initrdFile: true, kernelCmdline: 'root=/dev/vda2 ro quiet', clipboard: 'both', transfer: true,
      shares: [{ tag: 'docs', path: `${lord.fs.home()}/Documents`, writable: false }], guestFiles: true, guestNotify: true, sound: true, agentState: 'off' }
    : m.name === 'Base Debian' ? { ...m, secureBoot: true, tpm: true } : m));
  const host = lord.vm.host;
  lord.vm.host = () => ({ ...host(), missing: { amd64: 'passt', arm64: '' }, guestTools: { linux: true, windows: true } });
})();
