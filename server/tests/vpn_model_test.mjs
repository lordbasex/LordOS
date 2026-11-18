// vpn_model_test.mjs — the VPN widget's plain logic (sdk/vpnmodel.js): interface names as lord-netd gives them, a
// VPN's state from the interfaces, rates, which VPN a widget drives and the admin notices. node server/tests/vpn_model_test.mjs
import { ifnameOf, vpnState, rates, targetOf, eventOf } from '../../sdk/vpnmodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
eq('interface names (server/src/net/wg.c vpn_ifname)', [ifnameOf('wireguard', 'Office'), ifnameOf('openvpn', 'home'), ifnameOf('fortinet', 'corp'), ifnameOf('ipsec', 'averyveryverylongname')],
  ['wg-office', 'ovpn-home', 'fvpn-corp', 'ips-averyveryve']);
const ifs = [{ name: 'eth0', up: true, addresses: [{ family: 4, address: '192.168.1.5' }] },
  { name: 'wg-office', up: true, rxBytes: 5000, txBytes: 1200, addresses: [{ family: 6, address: 'fd00::2' }, { family: 4, address: '10.8.0.2' }] }];
eq('state: up, its IPv4 and counters', vpnState({ name: 'Office', type: 'wireguard' }, ifs), { up: true, ip: '10.8.0.2', rx: 5000, tx: 1200 });
eq('state: no interface, down', vpnState({ name: 'home', type: 'openvpn' }, ifs), { up: false, ip: '', rx: 0, tx: 0 });
eq('state: no VPN', vpnState(null, ifs).up, false);
eq('rates per second', rates({ rx: 1000, tx: 0 }, { rx: 5000, tx: 1200 }, 2), { rx: 2000, tx: 600 });
eq('rates: a counter reset is not negative', rates({ rx: 9000, tx: 9000 }, { rx: 10, tx: 10 }, 2), { rx: 0, tx: 0 });
eq('rates: no time', rates({ rx: 1, tx: 1 }, { rx: 2, tx: 2 }, 0), null);
const list = [{ name: 'Office' }, { name: 'home' }];
eq('target: the widget\'s own choice', targetOf({ vpn: 'home' }, list), 'home');
eq('target: a removed choice with several VPNs: none (ask)', targetOf({ vpn: 'gone' }, list), null);
eq('target: the only VPN', targetOf({}, [{ name: 'solo' }]), 'solo');
eq('target: none set up', targetOf({}, []), null);
eq('notices', [eventOf('net vpn Office connected'), eventOf('net vpn home need-auth'), eventOf('net link eth0 up')], [{ name: 'Office', what: 'connected' }, { name: 'home', what: 'need-auth' }, null]);
console.log(bad ? `vpn-model-test: ${bad} FAILED` : `vpn-model-test: ${ok} ok`);
if (bad) process.exit(1);
