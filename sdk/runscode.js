// runscode.js — pure (no `lord` needed, node-testable): whether a file's default opener would run it.
// A file whose default opener would RUN it (a launcher, a program, a script interpreter, Java, Wine, an installer
// package): never opened from content someone else could have put there (a note's attachment, a mail's). Shown in Files
// instead, where opening it is the person's explicit choice.
const RUNS_CODE_EXT = /\.(desktop|appimage|exe|msi|bat|cmd|com|scr|jar|jnlp|sh|bash|zsh|csh|py|pyw|pl|rb|php|js|mjs|ps1|vbs|run|bin|elf|deb|rpm|flatpakref|flatpakrepo|lpk|apk|x86_64|so)$/;
const RUNS_CODE_MIME = /^application\/(x-desktop|x-executable|x-pie-executable|x-sharedlib|x-shellscript|x-java-archive|java-archive|x-java-jnlp-file|x-ms-dos-executable|x-msdownload|x-msi|x-appimage|x-python|x-perl|x-ruby|x-php|vnd\.debian\.binary-package|x-rpm|vnd\.flatpak|x-sh)/;
export function runsCode(name) {
  const n = String(name || '').toLowerCase();
  if (RUNS_CODE_EXT.test(n)) return true;
  try { return RUNS_CODE_MIME.test(typeof lord !== 'undefined' && lord.mime ? String(lord.mime.guess(n) || '') : ''); } catch (e) { return false; }
}
