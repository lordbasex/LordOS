#!/usr/bin/env python3
# fake_logind.py — plays logind's sleep on a private bus for tests/lock_e2e.sh (issue #36).
#
#   python3 -I fake_logind.py <bus address> <sleep_at_s> <wake_after_s> <log>
#
# Owns org.freedesktop.login1 and answers Manager.Inhibit(what, who, why, mode) with the read end of a
# pipe, as logind does; it watches the write end to see when the caller lets the inhibitor go. <sleep_at_s>
# seconds after the first inhibitor it sends PrepareForSleep(true) and waits, as logind does for "delay"
# inhibitors, until every one is let go or 5 s pass; <wake_after_s> later it sends PrepareForSleep(false).
# A test tool only: it is never installed.
import os
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

ADDR, SLEEP_AT, WAKE_AFTER, LOG = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
T0 = time.monotonic()
log_f = open(LOG, 'a', buffering=1)


def say(msg):
    log_f.write(f'[{(time.monotonic() - T0) * 1000:7.0f} ms] logind: {msg}\n')


class Manager(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, '/org/freedesktop/login1')
        self.held = {}          # write end -> (who, why, since)
        self.asleep_at = None
        self.scheduled = False

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='ssss', out_signature='h')
    def Inhibit(self, what, who, why, mode):
        r, w = os.pipe()
        self.held[w] = (who, why, time.monotonic())
        say(f'Inhibit({what!r}, {who!r}, {why!r}, {mode!r}): inhibitor #{w} given')
        GLib.io_add_watch(w, GLib.PRIORITY_DEFAULT, GLib.IO_ERR | GLib.IO_HUP, self.released)
        fd = dbus.types.UnixFd(r)
        os.close(r)
        if not self.scheduled:
            self.scheduled = True
            GLib.timeout_add(int(SLEEP_AT * 1000), self.sleep)
        return fd

    def released(self, w, cond):
        who, why, since = self.held.pop(w)
        os.close(w)
        if self.asleep_at is not None:
            say(f'inhibitor #{w} let go {(time.monotonic() - self.asleep_at) * 1000:.0f} ms after PrepareForSleep(true)')
            if not self.held:
                say('no inhibitor left: the computer would sleep now')
        else:
            say(f'inhibitor #{w} let go (not sleeping)')
        return False

    @dbus.service.signal('org.freedesktop.login1.Manager', signature='b')
    def PrepareForSleep(self, start):
        pass

    def sleep(self):
        say(f'PrepareForSleep(true) with {len(self.held)} delay inhibitor(s) held')
        self.asleep_at = time.monotonic()
        self.PrepareForSleep(True)
        GLib.timeout_add(5000, self.max_delay)
        GLib.timeout_add(int(WAKE_AFTER * 1000), self.wake)
        return False

    def max_delay(self):
        old = [w for w, (_, _, since) in self.held.items() if self.asleep_at is not None and since <= self.asleep_at]
        if old:
            say(f'5 s passed with {len(old)} inhibitor(s) still held: the computer would sleep anyway')
        return False

    def wake(self):
        say('PrepareForSleep(false): awake')
        self.asleep_at = None
        self.PrepareForSleep(False)
        return False


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(ADDR)
name = dbus.service.BusName('org.freedesktop.login1', bus)
mgr = Manager(bus)
say('owns org.freedesktop.login1')
GLib.MainLoop().run()
