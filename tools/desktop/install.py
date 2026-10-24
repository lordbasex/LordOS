#!/usr/bin/env python3
"""Stage the desktop only. No host configuration or services are changed."""
import argparse
import json
import hashlib
from pathlib import Path
import shutil

REPO = Path(__file__).resolve().parents[2]


def install(prefix, libexecdir, destdir):
    prefix, libexecdir = Path(prefix), Path(libexecdir)
    if not prefix.is_absolute() or not libexecdir.is_absolute():
        raise ValueError('prefix and libexecdir must be absolute paths')
    if any(c.isspace() or c in "'\"\\" for c in str(prefix) + str(libexecdir)):
        raise ValueError('installation paths must not contain whitespace or shell quotes')
    root = libexecdir / 'lord-de'
    stage = Path(destdir) if destdir else Path('/')
    target = stage / root.relative_to('/')
    required = ['server/build/lord-server', 'server/build/lord-vault',
                'themes/default/theme.bundle.js', 'themes/classic/theme.bundle.js',
                'assets/app-host.bundle.js', 'code-manifest.json', 'server/build/gen/profile.h']
    for rel in required:
        if not (REPO / rel).is_file():
            raise ValueError(f'missing {rel}; run npm ci && make first')
    profile = (REPO / 'server/build/gen/profile.h').read_text()
    if 'profile=desktop' not in profile:
        raise ValueError('refusing to install a legacy OS build as Lord DE')
    if (REPO / 'build-profile.json').read_text().strip() != '"desktop"':
        raise ValueError('themes must be built with LORD_PROFILE=desktop')
    # Validate the complete install plan before copying any file.
    manifest = json.loads((REPO / 'code-manifest.json').read_text())
    code_paths = {f['path'] for f in manifest['files']}
    if any(p.startswith('themes/greeter/') for p in code_paths):
        raise ValueError('stale OS bundles found; rebuild the desktop themes')
    for file in manifest['files']:
        path = REPO / file['path']
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != file['sha256']:
            raise ValueError(f"stale or missing bundle: {file['path']}; rebuild the themes")

    def copy(src, dst, mode=None):
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        if mode is not None:
            dst.chmod(mode)

    for name in ['lord-server', 'lord-vault', 'lord-maild', 'lord-doc', 'lord-media']:
        src = REPO / 'server/build' / name
        if src.is_file():
            copy(src, target / 'bin' / name, 0o755)
    for name in ['fonts', 'locales', 'shell', 'icons', 'mascot', 'brand', 'monitor', 'help', 'wallpapers', 'gtk', 'store']:
        shutil.copytree(REPO / 'assets' / name, target / 'assets' / name, dirs_exist_ok=True)
    copy(REPO / 'assets/app-host.bundle.js', target / 'assets/app-host.bundle.js')
    shutil.copytree(REPO / 'assets/sysapps', target / 'assets/sysapps', dirs_exist_ok=True)
    theme_names = sorted({p.split('/')[1] for p in code_paths if p.startswith('themes/')})
    for name in theme_names:
        for file in ['theme.json', 'theme.bundle.js']:
            copy(REPO / 'themes' / name / file, target / 'themes' / name / file)
        assets = REPO / 'themes' / name / 'assets'
        if assets.exists():
            shutil.copytree(assets, target / 'themes' / name / 'assets', dirs_exist_ok=True)
    copy(REPO / 'code-manifest.json', target / 'code-manifest.json')
    if (REPO / 'code-manifest.sig').exists():
        copy(REPO / 'code-manifest.sig', target / 'code-manifest.sig')
    bindir = prefix / 'bin'
    launcher = stage / bindir.relative_to('/') / 'lord-de-session'
    launcher.parent.mkdir(parents=True, exist_ok=True)
    launcher.write_text((REPO / 'tools/desktop/lord-de-session.in').read_text().replace('@ROOT@', str(root)))
    launcher.chmod(0o755)
    session = stage / prefix.relative_to('/') / 'share/wayland-sessions/lord-de.desktop'
    session.parent.mkdir(parents=True, exist_ok=True)
    session.write_text((REPO / 'packaging/desktop/lord-de.desktop.in').read_text().replace('@BINDIR@', str(bindir)))
    for file in ['LICENSE', 'THIRD_PARTY.md', 'README.md']:
        copy(REPO / file, stage / prefix.relative_to('/') / 'share/doc/lord-de' / file)
    copy(REPO / 'docs/STANDALONE.md', stage / prefix.relative_to('/') / 'share/doc/lord-de/docs/STANDALONE.md')
    for file in ['LICENSE', 'LICENSE-SecLists']:
        for src in (REPO / 'third_party').rglob(file):
            copy(src, stage / prefix.relative_to('/') / 'share/licenses/lord-de/third_party' / src.relative_to(REPO / 'third_party'))
    print(f'Installed desktop in {target}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', default='/usr/local')
    parser.add_argument('--libexecdir', default='/usr/local/libexec')
    parser.add_argument('--destdir', default='')
    args = parser.parse_args()
    install(args.prefix, args.libexecdir, args.destdir)
