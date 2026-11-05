// vpnform_test.mjs — Settings → Network → VPN's form data (apps/settings/vpnform.js): PEM blocks,
// the edit form and the import lines. Invented values only. Run by tests/run_suites.sh (needs node).
import { pemBlock, PEM_CERT, PEM_KEY, fingerprint, formFromConfig, configLines } from '../../apps/settings/vpnform.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

const CERT = '-----BEGIN CERTIFICATE-----\nMIIBexample\n-----END CERTIFICATE-----';
const KEY = '-----BEGIN PRIVATE KEY-----\nMIIEexample\n-----END PRIVATE KEY-----';
const RSA = '-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,0011\n\nabc\n-----END RSA PRIVATE KEY-----';
const ENC = '-----BEGIN ENCRYPTED PRIVATE KEY-----\nxyz\n-----END ENCRYPTED PRIVATE KEY-----';

eq('a certificate file', pemBlock(`${CERT}\n`, PEM_CERT), CERT);
eq('Windows line ends', pemBlock(CERT.replace(/\n/g, '\r\n'), PEM_CERT), CERT);
eq('one file with both, text around', [pemBlock(`Bag Attributes\n x\n${CERT}\nmore\n${KEY}\n`, PEM_CERT), pemBlock(`Bag Attributes\n${CERT}\n${KEY}`, PEM_KEY)], [CERT, KEY]);
eq('an RSA key with its headers', pemBlock(RSA, PEM_KEY), RSA);
eq('an encrypted PKCS#8 key', pemBlock(ENC, PEM_KEY), ENC);
eq('no key in a certificate file', pemBlock(CERT, PEM_KEY), null);
eq('a public key is not a private key', pemBlock('-----BEGIN PUBLIC KEY-----\nx\n-----END PUBLIC KEY-----', PEM_KEY), null);
eq('mismatched END', pemBlock('-----BEGIN CERTIFICATE-----\nx\n-----END PRIVATE KEY-----', PEM_CERT), null);
eq('the first certificate of a chain', pemBlock(`${CERT}\n${CERT.replace('MIIBexample', 'second')}`, PEM_CERT), CERT);

eq('fingerprint', fingerprint('0a1b2c'), '0A:1B:2C');

const F = [['host'], ['port', null, null, null, false, true], ['alt-hosts', null, null, null, false, true, 'gateways'], ['auth', null, ['password', 'sso']],
  ['browser', null, null, null, false, true, 'info'], ['client-cert', null, null, null, false, true, 'certfiles'], ['key-password', null, null, null, true, true],
  ['description', null, null, null, false, true]];
eq('new: the fields that have a value', configLines(F, { host: ' vpn.example.test ', port: '443', auth: 'sso', gateways: ['b.example.test', ' ', 'c.example.test:4443'], description: 'Office' }),
  'host=vpn.example.test\nport=443\nalt-hosts=b.example.test,c.example.test:4443\nauth=sso\ndescription=Office');
eq('new: certificate and key as PEM values', configLines(F, { host: 'vpn.example.test', cert: CERT, key: KEY, 'key-password': 'k3y' }),
  `host=vpn.example.test\ncert=${CERT}\nkey=${KEY}\nkey-password=k3y`);
eq('new: a certificate without a key sends nothing', configLines(F, { host: 'vpn.example.test', cert: CERT }), 'host=vpn.example.test');
eq('new: no edit instructions', configLines(F, { host: 'vpn.example.test', removeClientCert: true, forgetCerts: true }), 'host=vpn.example.test');
eq('edit: remove the client certificate, forget trusted ones', configLines(F, { host: 'vpn.example.test', removeClientCert: true, forgetCerts: true }, true),
  'host=vpn.example.test\nremove-client-cert=1\nforget-certs=1');

const cfg = { type: 'fortinet', values: { host: 'vpn.example.test', 'alt-hosts': 'b.example.test, c.example.test:4443', auth: 'sso' }, files: ['cert', 'key'], secrets: ['key-password'] };
eq('edit form from net-vpn-config', formFromConfig(cfg, { port: '443', auth: 'password' }),
  { port: '443', auth: 'sso', host: 'vpn.example.test', gateways: ['b.example.test', 'c.example.test:4443'], hasFiles: true });
eq('edit form, no files', formFromConfig({ type: 'fortinet', values: { host: 'h.example.test' } }).hasFiles, false);
eq('edit form, another type untouched', formFromConfig({ type: 'pptp', values: { server: 's.example.test' } }), { server: 's.example.test' });

console.log(`vpnform: ${n - bad}/${n} ${bad ? 'FAILED' : 'ok'}`);
process.exit(bad ? 1 : 0);
