#!/usr/bin/python3
"""Use Firefox through Xwayland for Anaconda's live installer window."""
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    if not os.environ.get('DISPLAY'):
        raise SystemExit('The Lord DE session must provide its Xwayland display.')
    env = dict(os.environ, GDK_BACKEND='x11', MOZ_ENABLE_WAYLAND='0',
               MOZ_GTK_TITLEBAR_DECORATION='system')
    with tempfile.TemporaryDirectory(prefix='lord-de-installer-') as profile:
        shutil.copytree('/usr/share/anaconda/firefox-theme/live', profile, dirs_exist_ok=True)
        # Fullscreen also keeps the Dock's magnified hover area away from the
        # installer's navigation buttons on smaller displays.
        return subprocess.run(['/usr/bin/firefox', '--new-instance', '--kiosk',
                               '--profile', profile, *sys.argv[1:]],
                              env=env, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
