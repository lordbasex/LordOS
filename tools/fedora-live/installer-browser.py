#!/usr/bin/python3
"""Use Firefox through Xwayland for Anaconda's live installer window."""
import ctypes
import os
import shutil
import signal
import subprocess
import sys
import tempfile

PR_SET_PDEATHSIG = 1


def die_with_parent():
    # Firefox goes away with this wrapper, whatever ends it
    ctypes.CDLL(None, use_errno=True).prctl(PR_SET_PDEATHSIG, signal.SIGTERM)


def main():
    if not os.environ.get('DISPLAY'):
        raise SystemExit('The Axia DE session must provide its Xwayland display.')
    env = dict(os.environ, GDK_BACKEND='x11', MOZ_ENABLE_WAYLAND='0',
               MOZ_GTK_TITLEBAR_DECORATION='system')
    with tempfile.TemporaryDirectory(prefix='axia-installer-') as profile:
        shutil.copytree('/usr/share/anaconda/firefox-theme/live', profile, dirs_exist_ok=True)
        # Fullscreen also keeps the Dock's magnified hover area away from the
        # installer's navigation buttons on smaller displays.
        browser = subprocess.Popen(['/usr/bin/firefox', '--new-instance', '--kiosk',
                                    '--profile', profile, *sys.argv[1:]],
                                   env=env, preexec_fn=die_with_parent)
        # Anaconda closes its window by ending this wrapper ("Return to live", a finished or failed
        # installation): the kiosk window has no close button of its own, so Firefox must end too.
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(sig, lambda *_: browser.terminate())
        return browser.wait()


if __name__ == '__main__':
    raise SystemExit(main())
