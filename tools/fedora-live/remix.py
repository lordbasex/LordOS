#!/usr/bin/env python3
"""Remix a Fedora 44 x86_64 live ISO inside an isolated rootless container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
LABEL = 'Axia-DE-Live-44'


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(root, name, data, mode=0o644):
    path = root / name.lstrip('/')
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        path.unlink()
    path.write_text(data)
    path.chmod(mode)


def customize(root):
    # Keep GDM and the GNOME components needed by its greeter; hide the desktop
    # sessions so both live and new users enter Axia DE.
    for folder in ['usr/share/wayland-sessions', 'usr/share/xsessions']:
        for path in (root / folder).glob('gnome*.desktop'):
            text = re.sub(r'^Hidden=.*\n?', '', path.read_text(), flags=re.M)
            write(root, str(path.relative_to(root)), text.rstrip() + '\nHidden=true\n')
    session = root / 'usr/share/wayland-sessions/axia.desktop'
    text = session.read_text().replace('Exec=/usr/bin/axia-session',
                                       'Exec=/usr/libexec/axia/fedora-session')
    text = re.sub(r'^DesktopNames=.*\n?', '', text, flags=re.M)
    write(root, str(session.relative_to(root)), text + 'DesktopNames=Axia;\n')
    write(root, '/usr/libexec/axia/fedora-session', (HERE / 'session.py').read_text(), 0o755)
    write(root, '/usr/libexec/axia/installer-browser',
          (HERE / 'installer-browser.py').read_text(), 0o755)
    normal_gdm = '[daemon]\nDefaultSession=axia.desktop\nInitialSetupEnable=False\n'
    write(root, '/etc/gdm/custom.conf', normal_gdm)
    write(root, '/etc/sysconfig/livesys', 'livesys_session="axia"\n')
    write(root, '/usr/libexec/livesys/sessions.d/livesys-axia',
          (HERE / 'livesys-axia').read_text(), 0o755)
    write(root, '/etc/skel/.dmrc', '[Desktop]\nSession=axia\n')
    write(root, '/etc/skel/.config/gnome-initial-setup-done', '')
    # Modern GDM uses AccountsService's session rather than DefaultSession in
    # custom.conf. Seed the templates for users created later by Anaconda.
    for kind in ['standard', 'administrator']:
        path = root / f'usr/share/accountsservice/user-templates/{kind}'
        write(root, str(path.relative_to(root)),
              path.read_text().replace('Session=\n', 'Session=axia\nSessionType=wayland\n'))

    # Workstation hides account creation in favor of GNOME Initial Setup. The
    # independent desktop needs Anaconda to create the real administrator.
    write(root, '/etc/anaconda/conf.d/90-axia.conf',
          '[User Interface]\nhidden_webui_pages =\nhidden_spokes =\n'
          'webui_web_engine = /usr/libexec/axia/installer-browser\n')
    installer = root / 'usr/share/applications/liveinst.desktop'
    write(root, str(installer.relative_to(root)), installer.read_text().replace('NoDisplay=true', 'NoDisplay=false'))
    write(root, '/usr/share/anaconda/post-scripts/99-axia.ks',
          '%post --erroronfail\n'
          'cat > /etc/gdm/custom.conf <<\'EOF\'\n' + normal_gdm + 'EOF\n'
          'rm -f /usr/share/polkit-1/rules.d/20-livesys-axia.rules\n'
          'systemctl set-default graphical.target\n'
          '%end\n')
    write(root, '/usr/share/doc/axia-de/ISO-README.txt',
          'Fedora 44 + Axia DE 0.8.7 (experimental, unofficial remix).\n'
          'GDM is retained for login; Axia DE is the default desktop.\n'
          'Use Install to Hard Drive in the app launcher, or run liveinst.\n'
          'The installer creates the account for the installed system.\n'
          'For Wi-Fi, run nm-connection-editor or nmtui in Terminal.\n')

    # Restore package capabilities lost by unprivileged EROFS extraction. Linux
    # translates namespaced file capabilities back to v2 on read in this same
    # user namespace; mkfs.erofs serializes those for root in the booted image.
    result = subprocess.check_output(['rpm', '--root', str(root), '-qa',
                                      '--qf', '[%{FILENAMES}\t%{FILECAPS}\n]'], text=True)
    caps = []
    for line in result.splitlines():
        name, _, value = line.partition('\t')
        if value and value != '(none)' and (root / name.lstrip('/')).is_file():
            run('chroot', root, '/usr/sbin/setcap', value, name)
            caps.append({'path': name, 'capabilities': value})
    return caps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--rpm', type=Path, required=True)
    parser.add_argument('--workdir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume-root', action='store_true', help='Reuse an already extracted root in workdir')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run through build.sh, which provides root in a user namespace.')
    if args.output.exists() or args.output.resolve() == args.iso.resolve():
        parser.error('The output must be a new file, distinct from the source ISO.')
    work = args.workdir.resolve()
    root = work / 'rootfs'
    work.mkdir(parents=True, exist_ok=True)
    original = work / 'original'
    original.mkdir(exist_ok=True)
    if not args.resume_root:
        if root.exists():
            parser.error('Use a new workdir or --resume-root for an existing extraction.')
        run('xorriso', '-osirrox', 'on', '-indev', args.iso,
            '-extract', '/LiveOS/squashfs.img', original / 'squashfs.img',
            '-extract', '/boot/grub2/grub.cfg', original / 'grub.cfg')
        run('fsck.erofs', f'--extract={root}', '--preserve', original / 'squashfs.img')
    release = (root / 'etc/fedora-release').read_text()
    if '44 ' not in release:
        parser.error(f'Only Fedora 44 is supported: {release}')
    run('dnf', '-y', f'--installroot={root}', '--releasever=44',
        '--setopt=install_weak_deps=False', 'install', args.rpm, 'mate-polkit',
        'NetworkManager-tui', 'nm-connection-editor', 'xorg-x11-server-Xwayland')
    run('dnf', '-y', f'--installroot={root}', '--releasever=44',
        'remove', '--no-autoremove', 'gnome-initial-setup')
    caps = customize(root)
    run('dnf', f'--installroot={root}', '--releasever=44', 'clean', 'all')
    # Build-time package downloads and machine identities do not belong on media.
    write(root, '/etc/machine-id', '')
    for folder in ['var/cache/libdnf5', 'var/cache/dnf']:
        directory = root / folder
        for item in directory.iterdir() if directory.is_dir() else []:
            if item.is_dir() and not item.is_symlink():
                shutil.rmtree(item)
            else:
                item.unlink()
    # Keep log directories and their ownership/modes so Fedora services start
    # with the expected SELinux contexts, even on their first log write.
    for path in (root / 'var/log').rglob('*'):
        if path.is_file() and not path.is_symlink():
            path.unlink()
    grub = (original / 'grub.cfg').read_text().replace('Fedora-WS-Live-44', LABEL)
    grub = grub.replace('Fedora-Workstation-Live', 'Fedora-Axia-DE-Live').replace('set default="1"', 'set default="0"')
    (work / 'grub.cfg').write_text(grub)
    # Generate target SELinux labels directly; host SELinux labels on the
    # rootless extraction must never be copied into the live filesystem.
    run('mkfs.erofs', '-zlz4hc,level=12', '-C65536', '--workers=8',
        f'--file-contexts={root}/etc/selinux/targeted/contexts/files/file_contexts',
        work / 'squashfs.img', root)
    run('fsck.erofs', '--extract', work / 'squashfs.img')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run('xorriso', '-indev', args.iso, '-outdev', args.output,
        '-volid', LABEL, '-overwrite', 'on',
        '-map', work / 'squashfs.img', '/LiveOS/squashfs.img',
        '-map', work / 'grub.cfg', '/boot/grub2/grub.cfg',
        '-boot_image', 'any', 'replay', '-commit', '-end')
    run('implantisomd5', '--force', args.output)
    run('checkisomd5', args.output)
    sha = digest(args.output)
    args.output.with_suffix('.iso.sha256').write_text(f'{sha}  {args.output.name}\n')
    metadata = {'base': args.iso.name, 'base_sha256': digest(args.iso),
                'rpm': args.rpm.name, 'rpm_sha256': digest(args.rpm),
                'iso': args.output.name, 'iso_sha256': sha, 'volume_id': LABEL,
                'login_manager': 'GDM', 'default_session': 'axia',
                'restored_file_capabilities': caps}
    args.output.with_suffix('.iso.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Created {args.output}', flush=True)


if __name__ == '__main__':
    main()
