// quarantine.js — a file a Machines guest sent (docs/VIRTUALIZATION.md 6.21; lord.fs.stat's quarantine, which what comes
// out of such an archive carries too): one that would run or install something here when opened (a program, an
// installer, a desktop entry, a package, a script for another system) is never opened by a double click; Open With still
// opens it when the person picks an app. Pure, tested in server/tests/vm_xfer_test.mjs.
import { isProgram, isForeignProgram } from '../../sdk/filekinds.js';

const INSTALLS = /\.(deb|rpm|appimage|run|flatpak|flatpakref|flatpakrepo|snap|desktop|lpk|exe|msi|bat|cmd|com|scr|ps1|vbs|jar)$/i;
export function quarantineBlocks(e, st) {
  return !!(e && st && st.quarantine && !e.dir && (INSTALLS.test(String(e.name || '')) || isProgram(e) || isForeignProgram(e)));
}
