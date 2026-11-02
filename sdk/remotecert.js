// remotecert.js — what the RDP app does with the certificate a server shows (docs/FILES.md, "Remote desktop"), no screen:
// the app keeps the fingerprints it accepted, and only it can tell a known certificate from a changed one.
export function certCheck(trusted, key, cert) {
  const old = trusted[key];
  if (!old) return { kind: 'new' };
  return old === cert ? { kind: 'known' } : { kind: 'changed', old };
}
