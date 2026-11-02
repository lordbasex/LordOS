#!/usr/bin/python3
"""Run Lord DE with Fedora's user bus and an independent polkit agent."""
import os
from pathlib import Path
import signal
import stat
import subprocess
import time


def sockets(runtime):
    return {p.name for p in runtime.glob('wayland-*') if stat.S_ISSOCK(p.stat().st_mode)}


def x_display(pid):
    # Lord DE reserves an Xwayland socket before starting Xwayland on demand.
    # Match its lock to this compositor, rather than guessing a display number.
    for lock in Path('/tmp').glob('.X*-lock'):
        try:
            number = lock.name[2:-5]
            sock = Path('/tmp/.X11-unix') / f'X{number}'
            if (number.isdigit() and lock.stat().st_uid == os.getuid()
                    and int(lock.read_text().strip()) == pid
                    and sock.stat().st_uid == os.getuid()
                    and stat.S_ISSOCK(sock.stat().st_mode)):
                return f':{number}'
        except (OSError, ValueError):
            continue
    return ''


def main():
    if os.geteuid() == 0:
        raise SystemExit('Start Lord DE as a regular user.')
    if not os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
        os.execvp('dbus-run-session', ['dbus-run-session', '--', __file__])
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    before = sockets(runtime)
    env = dict(os.environ, XDG_CURRENT_DESKTOP='LordOS', XDG_SESSION_DESKTOP='lord-de',
               XDG_SESSION_TYPE='wayland', GDK_BACKEND='wayland', MOZ_ENABLE_WAYLAND='1',
               DISPLAY='')
    server = subprocess.Popen(['/usr/bin/lord-de-session'], env=env)
    helpers = []

    def stop(signum, frame):
        if server.poll() is None:
            server.send_signal(signum)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        for _ in range(200):
            if server.poll() is not None:
                return server.returncode
            new = sockets(runtime) - before
            display = x_display(server.pid)
            if new and display:
                env['WAYLAND_DISPLAY'] = sorted(new)[0]
                env['DISPLAY'] = display
                names = ['DISPLAY', 'WAYLAND_DISPLAY', 'XDG_CURRENT_DESKTOP', 'XDG_SESSION_DESKTOP',
                         'XDG_SESSION_TYPE', 'GDK_BACKEND', 'MOZ_ENABLE_WAYLAND']
                subprocess.run(['dbus-update-activation-environment', '--systemd', *names],
                               env=env, check=False, timeout=10)
                agent = '/usr/libexec/polkit-mate-authentication-agent-1'
                if Path(agent).is_file():
                    helpers.append(subprocess.Popen([agent], env=env))
                break
            time.sleep(0.1)
        return server.wait()
    finally:
        for child in helpers:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


if __name__ == '__main__':
    raise SystemExit(main())
