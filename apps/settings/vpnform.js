// vpnform.js — the VPN form's data, without a screen (Settings → Network → VPN, Network.jsx):
// PEM blocks from the files the person picks, the edit form's values from net-vpn-config and the
// import lines sent to lord-netd (docs/NETWORK.md, "Import formats"). Tested by
// server/tests/vpnform_test.mjs.

// The first PEM block of a kind in a text (a file may hold the certificate and the key, or other
// lines around them, as an export with "Bag Attributes" does), or null.
export const pemBlock = (text, kind) => {
  const m = new RegExp(`-----BEGIN (${kind})-----[\\s\\S]*?-----END \\1-----`).exec(String(text).replace(/\r/g, ''));
  return m ? m[0] : null;
};
export const PEM_CERT = 'CERTIFICATE';
export const PEM_KEY = '(?:RSA |EC |ENCRYPTED )?PRIVATE KEY';

// 64 hex digits → "AB:CD:…", the way certificate fingerprints are usually shown
export const fingerprint = (hex) => String(hex).toUpperCase().match(/../g)?.join(':') || '';

// The edit form's starting values from net-vpn-config ({ type, values, files }).
export function formFromConfig(edit, defaults = {}) {
  const f = { ...defaults, ...edit.values };
  if (edit.type === 'fortinet') {
    f.gateways = String(edit.values['alt-hosts'] || '').split(',').map((x) => x.trim()).filter(Boolean);
    delete f['alt-hosts'];
    f.hasFiles = (edit.files || []).includes('cert');
  }
  return f;
}

// One "key=value" line per field (a PEM value continues on the next lines). fields: the form's
// rows that are shown, [key, label, options, show, secret, optional, kind].
export function configLines(fields, form, edit) {
  const out = [];
  for (const [k, , , , , , kind] of fields) {
    if (kind === 'info') continue;
    if (kind === 'gateways') {
      const v = (form.gateways || []).map((x) => String(x).trim()).filter(Boolean);
      if (v.length) out.push(`alt-hosts=${v.join(',')}`);
    } else if (kind === 'certfiles') {
      if (form.cert && form.key) out.push(`cert=${form.cert}`, `key=${form.key}`);
      else if (edit && form.removeClientCert) out.push('remove-client-cert=1');
    } else if (String(form[k] || '').trim()) out.push(`${k}=${String(form[k]).trim()}`);
  }
  if (edit && form.forgetCerts) out.push('forget-certs=1');
  return out.join('\n');
}
