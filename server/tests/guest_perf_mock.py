# guest_perf_mock.py — stand-ins for tests/guest_perf_test.sh: power-profiles-daemon on a private "system" bus and the
# desktop portal's Settings on a private session bus (the value it reads is the real one, through GSettings and the
# real dconf-service the session bus starts). Test code only, never installed.
#   python3 -I guest_perf_mock.py SYSTEM_ADDR SESSION_ADDR MODE_FILE
# MODE_FILE holds words read at each call: "refuse" (Set is refused), "oldportal" (the portal has no ReadOne, only Read
# with a variant in a variant); and at the start: "hadess" (only the older name is there).
import sys
from gi.repository import Gio, GLib

sys_addr, ses_addr, mode_file = sys.argv[1:4]

def mode():
    try:
        with open(mode_file) as f:
            return f.read().split()
    except OSError:
        return []

PPD = [('org.freedesktop.UPower.PowerProfiles', '/org/freedesktop/UPower/PowerProfiles'), ('net.hadess.PowerProfiles', '/net/hadess/PowerProfiles')]
state = {'profile': 'balanced'}
want = {'n': 0}
def acquired(conn, name):
    want['n'] -= 1
    if not want['n']:
        print('ready', flush=True)

def ppd_xml(name):
    return Gio.DBusNodeInfo.new_for_xml(
        '<node><interface name="%s"><property name="ActiveProfile" type="s" access="readwrite"/></interface></node>' % name).interfaces[0]

def connect(addr):
    flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
    return Gio.DBusConnection.new_for_address_sync(addr, flags, None, None)

sysc = connect(sys_addr)
for i, (name, path) in enumerate(PPD):
    if i == 0 and 'hadess' in mode():
        continue
    def get_prop(conn, sender, path, iface, prop):
        return GLib.Variant('s', state['profile'])
    def set_prop(conn, sender, path, iface, prop, value):
        if 'refuse' in mode():
            return False
        v = value.unpack()
        if v not in ('power-saver', 'balanced', 'performance'):
            return False
        state['profile'] = v
        return True
    sysc.register_object(path, ppd_xml(name), None, get_prop, set_prop)
    want['n'] += 1
    Gio.bus_own_name_on_connection(sysc, name, Gio.BusNameOwnerFlags.NONE, acquired, None)

sesc = connect(ses_addr)
portal = Gio.DBusNodeInfo.new_for_xml('''<node><interface name="org.freedesktop.portal.Settings">
<method name="ReadOne"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
<method name="Read"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
</interface></node>''').interfaces[0]

def portal_call(conn, sender, path, iface, method, params, inv):
    ns, key = params.unpack()
    if method == 'ReadOne' and 'oldportal' in mode():
        inv.return_dbus_error('org.freedesktop.DBus.Error.UnknownMethod', 'no ReadOne')
        return
    if ns != 'org.gnome.desktop.interface' or key != 'enable-animations':
        inv.return_dbus_error('org.freedesktop.portal.Error.NotFound', 'not here')
        return
    v = GLib.Variant('b', Gio.Settings.new('org.gnome.desktop.interface').get_boolean('enable-animations'))
    if method == 'Read':
        v = GLib.Variant('v', v)
    inv.return_value(GLib.Variant('(v)', (v,)))

sesc.register_object('/org/freedesktop/portal/desktop', portal, portal_call, None, None)
want['n'] += 1
Gio.bus_own_name_on_connection(sesc, 'org.freedesktop.portal.Desktop', Gio.BusNameOwnerFlags.NONE, acquired, None)
GLib.MainLoop().run()
