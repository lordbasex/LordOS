#!/usr/bin/env python3
# Rewrites an exported .lordvm's machine.json with every host-boundary setting at its most permissive value and makes
# its manifest.json match again (a hostile export that passes the checksum check): tests/vm_storage_test.sh then
# imports it and checks the result is a new machine's safe defaults. usage: vm_export_hostile.py EXPORT_DIR
import hashlib, json, os, sys

d = sys.argv[1]
mj = os.path.join(d, 'machine.json')
m = json.load(open(mj))
m.update({
    'agent': True, 'clipboard': 'both', 'clipboard_ask': False, 'transfer': True, 'microphone': True,
    'sound_out': 'alsa_output.evil', 'sound_in': 'alsa_input.evil', 'capture_fullscreen': True, 'pointer': 'guest',
    'perf_profile': True, 'cpu_pin': 'auto', 'guest_links': True, 'guest_notify': True, 'automation': True,
    'usb_auto': ['046d:c52b'], 'graphics': '3d', 'network': 'shared', 'disposable': 'disk', 'backup': False,
    'ports': [{'proto': 'tcp', 'host': 2222, 'guest': 22, 'open': 'lan'}],
    'shares': [{'tag': 'home', 'path': os.path.expanduser('~'), 'writable': True}],
    'iso': '/etc/shadow',
})
open(mj, 'w').write(json.dumps(m, indent=2) + '\n')
man = json.load(open(os.path.join(d, 'manifest.json')))
for f in man['files']:
    p = os.path.join(d, f['path'])
    f['size'] = os.path.getsize(p)
    f['sha256'] = hashlib.sha256(open(p, 'rb').read()).hexdigest()
open(os.path.join(d, 'manifest.json'), 'w').write(json.dumps(man, indent=2) + '\n')
