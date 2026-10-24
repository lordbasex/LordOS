#!/usr/bin/env python3
"""Check that built/staged artifacts form an independent desktop."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from install import install, REPO


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs).stdout


def main():
    assert json.loads((REPO / 'build-profile.json').read_text()) == 'desktop'
    symbols = run('nm', str(REPO / 'server/build/lord-server'))
    for symbol in ['pkg_act_install', 'pkg_act_update', 'install_act_run', 'net_act_restore',
                   'act_langpack', 'js_pkg_register', 'js_imager_register']:
        assert symbol not in symbols, f'OS component linked: {symbol}'
    assert 'libseat_open_seat' in symbols, 'desktop must use a host seat backend'
    manifest = json.loads((REPO / 'code-manifest.json').read_text())
    for file in manifest['files']:
        path = REPO / file['path']
        assert path.stat().st_size == file['size']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == file['sha256']
        assert not file['path'].startswith('themes/greeter/')
    for name in ['default', 'classic']:
        bundle = (REPO / f'themes/{name}/theme.bundle.js').read_text()
        assert 'pkg-install-file' not in bundle, 'Debian store bundled into desktop'
        assert 'install-run' not in bundle, 'OS installer bundled into desktop'
    with tempfile.TemporaryDirectory(prefix='lord-de-stage-') as tmp:
        install('/usr', '/usr/libexec', tmp)
        staged = Path(tmp) / 'usr/libexec/lord-de'
        assert set(p.name for p in (staged / 'bin').iterdir()) <= {
            'lord-server', 'lord-vault', 'lord-maild', 'lord-doc', 'lord-media'}
        assert not (Path(tmp) / 'etc').exists()
        assert not (Path(tmp) / 'usr/lib/systemd').exists()
        assert (Path(tmp) / 'usr/share/wayland-sessions/lord-de.desktop').is_file()
        assert 'FAILED' not in run(str(staged / 'bin/lord-server'), '--check-code', str(staged))
        Path(tmp).chmod(0o755)
        # An ordinary user can initialize the complete desktop without root or a seat
        # in headless mode. This does not validate DRM/KMS or physical input.
        with tempfile.TemporaryDirectory(prefix='lord-de-home-') as tmp:
            home = Path(tmp)
            (home / 'runtime').mkdir(mode=0o700)
            env = dict(os.environ, HOME=tmp, USER='nobody', XDG_RUNTIME_DIR=str(home / 'runtime'))
            for key in ['SUDO_USER', 'DBUS_SESSION_BUS_ADDRESS', 'WAYLAND_DISPLAY', 'DISPLAY']:
                env.pop(key, None)
            opts = {}
            if os.geteuid() == 0:
                os.chown(home, 65534, 65534)
                os.chown(home / 'runtime', 65534, 65534)
                opts = {'user': 65534, 'group': 65534, 'extra_groups': []}
            for theme in ['default', 'classic']:
                try:
                    run(str(staged / 'bin/lord-server'), '--headless', '800x600', '--timeout', '1',
                        '--theme', f'../themes/{theme}/theme.bundle.js',
                        cwd=staged / 'bin', env=env, timeout=15, **opts)
                except subprocess.CalledProcessError as error:
                    log = home / '.cache/lordos/session.log'
                    print(error.stderr)
                    if log.exists():
                        print(log.read_text())
                    raise
                log = (home / '.cache/lordos/session.log').read_text()
                assert f'js: theme ../themes/{theme}/theme.bundle.js loaded' in log, log
                for error in ['ReferenceError:', 'TypeError:', 'SyntaxError:', 'code refused']:
                    assert error not in log, log
    print('desktop: no OS backend, manifest valid, installation isolated, headless startup passed')


if __name__ == '__main__':
    main()
