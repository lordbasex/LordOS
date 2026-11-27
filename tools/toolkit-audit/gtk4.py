#!/usr/bin/python3
# tools/toolkit-audit/gtk4.py — the GTK 4 harness of "lordos-testbench --toolkits" (installed in
# /usr/lib/lordos/toolkit-audit/). It opens one standard window with PyGObject's GTK 4, checks from inside what a
# GTK 4 app sees on this compositor, prints ONE JSON line on stdout and exits by itself within 5 s.
#
#   GDK_BACKEND=wayland|x11 WAYLAND_DEBUG=1 python3 -I gtk4.py
#
# LordOS does not depend on GTK or PyGObject: when python3-gi or the Gtk 4.0 typelib is missing, the line says
# {"installed": false} and the exit code is 3. Nothing is ever installed. The backend is the one GDK_BACKEND
# names (the orchestrator sets it); WAYLAND_DEBUG goes to stderr, where the orchestrator reads the globals bound.
#
# Output: {"toolkit":"gtk4","version":"4.x.y","backend":"wayland"|"x11"|null,
#          "checks":{id:{"ok":true|false|null,"ms"?:n,"detail"?:str}}}
# with the ids every harness shares: map size scale theme backend decoration first_frame menu combo tooltip
# clipboard dnd text fullscreen. "ok": null means skipped, and "detail" says why. Times are milliseconds from the
# start of this script. Privacy: only toolkit names and versions, the backend, numbers and fixed words; never a
# user name, a path, a window title or an error text (an exception is reported by its type name).
# Exit codes: 0 ran, 2 no display could be opened, 3 the bindings are not installed.
#
# Show mode (--show, or LORDOS_TOOLKIT_AUDIT_SHOW=1; the test bench's Frameworks screen): the same checks and the same
# JSON line, slowed down so a person can follow them (each popup held open about 0.9 s, the text typed one character
# at a time, fullscreen held on and off), the current step's id in a label on the window (a check id, never
# translated), and before the JSON line one progress line per step and per check on stdout: "#lordos-ta step <id>"
# and "#lordos-ta check <id> pass|fail|skip". The deadline is then 14.6 s (the orchestrator kills at 20 s).
import json
import os
import sys
import time

T0 = time.monotonic()
TOOLKIT = 'gtk4'
IDS = ('map', 'size', 'scale', 'theme', 'backend', 'decoration', 'first_frame', 'menu', 'combo', 'tooltip',
       'clipboard', 'dnd', 'text', 'fullscreen')
REQ_W, REQ_H = 600, 400
SHOW = '--show' in sys.argv[1:] or os.environ.get('LORDOS_TOOLKIT_AUDIT_SHOW') == '1'
DEADLINE_MS = 14600 if SHOW else 4600   # everything still pending then is reported as a timeout, and the harness exits
# A first frame later than 1 s is the toolkit's own start (GTK 4 compiling its shaders on a cold cache, 2.5 to 3 s on
# a software renderer, while the compositor had answered in a few ms): that much more time, up to LATE_MAX_MS, so the
# checks after it are still measured. The orchestrator kills at 8 s (20 s).
LATE_MAX_MS = 2400
HOLD_MS = 900 if SHOW else 0            # show mode: how long a step stays on screen once decided
SAMPLE = 'LordOS-audit-0123'


def ms():
    return int((time.monotonic() - T0) * 1000)


def progress(line):
    """Show mode: one progress line on stdout, before the JSON line."""
    if SHOW:
        sys.stdout.write('#lordos-ta ' + line + '\n')
        sys.stdout.flush()


def emit(version, backend, checks, code):
    out = {'toolkit': TOOLKIT, 'version': version, 'backend': backend, 'checks': {}}
    for i in IDS:
        out['checks'][i] = checks.get(i) or {'ok': False, 'detail': 'not reached'}
    if code == 3:
        out['installed'] = False
    sys.stdout.write(json.dumps(out, separators=(',', ':')) + '\n')
    sys.stdout.flush()


try:
    import gi
    gi.require_version('Gtk', '4.0')
    gi.require_version('Gdk', '4.0')
    from gi.repository import GLib
    # the window's own app id and X class (never "python3" or the script's name), set before GTK opens the display:
    # the shell keeps no window memory for lordos-toolkit-audit* (sdk/shellxmodel.js remembersPlace)
    GLib.set_prgname('lordos-toolkit-audit-gtk4')
    from gi.repository import Gdk, Gio, GLib, GObject, Gtk
except (ImportError, ValueError):
    emit(None, None, {i: {'ok': None, 'detail': 'skipped: not installed'} for i in IDS}, 3)
    sys.exit(3)

VERSION = '%d.%d.%d' % (Gtk.get_major_version(), Gtk.get_minor_version(), Gtk.get_micro_version())


def backend_of(display):
    name = GObject.type_name(display.__gtype__) if display else ''
    if 'Wayland' in name:
        return 'wayland'
    if 'X11' in name:
        return 'x11'
    return name.replace('Gdk', '').replace('Display', '').lower() or None


class Audit:
    def __init__(self):
        self.checks = {}
        self.done = False
        self.mapped_at = None
        self.display = Gdk.Display.get_default()
        self.backend = backend_of(self.display)
        self.loop = GLib.MainLoop()
        self.steps = []
        self.build()

    # ------------------------------------------------------------------ results
    def put(self, cid, ok, detail=None, t=None):
        if cid in self.checks:
            return
        r = {'ok': ok}
        if t is not None:
            r['ms'] = t
        if detail:
            r['detail'] = detail
        self.checks[cid] = r
        if cid in IDS and not self.done:
            progress('check %s %s' % (cid, 'skip' if ok is None else 'pass' if ok is True else 'fail'))

    def mark(self, cid):
        """Show mode: the step now on screen, in the label and as a progress line."""
        if not SHOW or self.done:
            return
        progress('step ' + cid)
        if self.step_label is not None:
            self.step_label.set_markup('<span font_family="monospace" weight="bold" size="large">%s</span>' % cid)

    def held(self, fn, ms=None):
        """fn, run HOLD_MS later in show mode (the step stays on screen), at once otherwise."""
        if not SHOW:
            return fn
        return lambda *_: GLib.timeout_add(max(1, HOLD_MS if ms is None else ms), lambda: fn() and False) and False

    def deadline(self):
        ff = self.checks.get('first_frame', {}).get('ms')
        return DEADLINE_MS + (min(LATE_MAX_MS, ff - 1000) if isinstance(ff, int) and ff > 1000 else 0)

    def on_deadline(self, *_):
        if not self.done and ms() < self.deadline():
            GLib.timeout_add(max(1, self.deadline() - ms()), self.on_deadline)
            return False
        return self.finish()

    def finish(self, *_):
        if self.done:
            return False
        for i in IDS:
            if i not in self.checks:
                self.put(i, False, 'timeout')
        self.done = True
        emit(VERSION, self.backend, self.checks, 0)
        self.loop.quit()
        return False

    def next_step(self, *_):
        if self.done:
            return False
        if not self.steps:
            self.finish()
            return False
        step = self.steps.pop(0)
        name = step.__name__.replace('step_', '')
        self.mark('size' if name == 'static' else name)
        try:
            step()
        except Exception as e:  # a step that throws fails alone; the next one still runs
            self.put(step.__name__.replace('step_', ''), False, 'error: ' + type(e).__name__)
            GLib.idle_add(self.next_step)
        return False

    def after(self, delay, fn):
        GLib.timeout_add(delay, fn)

    # ------------------------------------------------------------------ window
    def build(self):
        w = self.win = Gtk.Window()
        w.set_title('toolkit-audit')           # fixed; never reported
        w.set_default_size(REQ_W, REQ_H)

        hb = Gtk.HeaderBar()
        hb.pack_start(Gtk.Button.new_from_icon_name('go-previous-symbolic'))
        menu = Gio.Menu()
        for n in ('1', '2', '3'):
            menu.append(n, 'app.none')
        self.menubtn = Gtk.MenuButton(icon_name='open-menu-symbolic', menu_model=menu)
        hb.pack_end(self.menubtn)
        self.theme_icon = Gtk.Image.new_from_icon_name('weather-clear-symbolic')
        hb.pack_end(self.theme_icon)       # the light or dark indicator
        w.set_titlebar(hb)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        for side in ('start', 'end', 'top', 'bottom'):
            getattr(box, 'set_margin_' + side)(12)
        top = Gtk.CenterBox()                  # strictly start- and end-aligned buttons
        b1 = Gtk.Button.new_from_icon_name('list-add-symbolic')
        b1.set_halign(Gtk.Align.START)
        b2 = Gtk.Button.new_from_icon_name('list-remove-symbolic')
        b2.set_halign(Gtk.Align.END)
        self.tip_anchor = b2
        b2.set_tooltip_text('1')
        top.set_start_widget(b1)
        top.set_end_widget(b2)
        box.append(top)

        self.entry = Gtk.Entry()
        box.append(self.entry)
        # show mode: the current step's id (a fixed check id, not translated)
        self.step_label = None
        if SHOW:
            self.step_label = Gtk.Label(halign=Gtk.Align.START)
            self.step_label.set_markup('<span font_family="monospace" weight="bold" size="large">map</span>')
            box.append(self.step_label)
        box.append(Gtk.CheckButton())
        self.scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 1)
        self.scale.set_value(40)
        box.append(self.scale)
        self.combo = Gtk.DropDown.new_from_strings(['1', '2', '3'])
        self.combo.set_halign(Gtk.Align.START)
        box.append(self.combo)
        w.set_child(box)

        w.connect('realize', self.on_realize)
        w.connect('close-request', lambda *_: self.finish() or False)

    def on_realize(self, w):
        surf = w.get_surface()
        surf.connect('notify::mapped', self.on_surface_mapped)
        clock = w.get_frame_clock()
        self.paint_id = clock.connect('after-paint', self.on_after_paint)

    def on_surface_mapped(self, surf, _pspec):
        if surf.get_mapped() and self.mapped_at is None:
            self.mapped_at = ms()
            self.put('map', True, None, self.mapped_at)

    def on_after_paint(self, clock):
        if self.mapped_at is None or 'first_frame' in self.checks:
            return
        self.put('first_frame', True, None, ms())
        clock.disconnect(self.paint_id)
        # let the first configure settle before measuring
        self.after(150, self.next_step)

    # ------------------------------------------------------------------ checks
    def step_static(self):
        w = self.win
        surf = w.get_surface()
        ww, wh = win_size(w)
        self.put('size', ww == REQ_W and wh == REQ_H, '%dx%d' % (ww, wh))

        sc = surf.get_scale() if hasattr(surf, 'get_scale') else float(surf.get_scale_factor())
        mon = self.display.get_monitor_at_surface(surf)
        msc = None
        if mon is not None:
            msc = mon.get_scale() if hasattr(mon, 'get_scale') else float(mon.get_scale_factor())
        if msc is None:
            self.put('scale', sc > 0, 'scale=%g' % sc)
        else:
            self.put('scale', sc > 0 and abs(sc - msc) < 0.01, 'scale=%g monitor=%g' % (sc, msc))

        want = (GLib.getenv('GDK_BACKEND') or '').split(',')[0].strip()
        if want and want != '*':
            self.put('backend', self.backend == want, self.backend)
        else:
            self.put('backend', self.backend is not None, self.backend)

        if w.has_css_class('csd'):
            deco = 'csd'
        elif w.has_css_class('solid-csd'):
            deco = 'solid-csd'
        else:
            deco = 'ssd' if w.get_decorated() else 'none'
        hb = w.get_titlebar()
        self.put('decoration', w.get_decorated() and hb is not None and hb.get_mapped(), deco)
        GLib.idle_add(self.held(self.next_step, HOLD_MS // 2))

    def step_theme(self):
        # the desktop's color scheme through the Settings portal, and what GTK itself was told
        settings = Gtk.Settings.get_default()
        dark_tk = bool(settings.get_property('gtk-application-prefer-dark-theme'))
        name = (settings.get_property('gtk-theme-name') or '').lower()
        dark_tk = dark_tk or name.endswith('-dark') or name.endswith(':dark')
        t = ms()

        def done(scheme, how):
            if 'theme' in self.checks:
                return
            words = {0: 'default', 1: 'dark', 2: 'light'}
            self.theme_icon.set_from_icon_name(
                'weather-clear-night-symbolic' if (scheme == 1 or (scheme is None and dark_tk))
                else 'weather-clear-symbolic')
            if scheme in words:
                self.put('theme', True, 'portal=%s toolkit=%s' % (words[scheme], 'dark' if dark_tk else 'light'),
                         ms() - t)
            else:
                self.put('theme', False, '%s toolkit=%s' % (how, 'dark' if dark_tk else 'light'), ms() - t)
            GLib.idle_add(self.held(self.next_step, HOLD_MS // 2))

        def unpack(v):
            while isinstance(v, GLib.Variant):
                v = v.unpack()
            while isinstance(v, tuple) and len(v) == 1:
                v = v[0]
                while isinstance(v, GLib.Variant):
                    v = v.unpack()
            return v if isinstance(v, int) else None

        def on_reply(bus, res, method):
            try:
                v = bus.call_finish(res)
                done(unpack(v), 'portal')
            except GLib.Error:
                if method == 'ReadOne':
                    call('Read')
                else:
                    done(None, 'portal=unavailable')

        def call(method):
            bus.call('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                     'org.freedesktop.portal.Settings', method,
                     GLib.Variant('(ss)', ('org.freedesktop.appearance', 'color-scheme')),
                     None, Gio.DBusCallFlags.NONE, 800, None, on_reply, method)

        try:
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        except GLib.Error:
            done(None, 'portal=no-session-bus')
            return
        call('ReadOne')

    def wait_popup(self, cid, popover, start, stop, limit=900, ok_detail=None):
        """open a popup, and pass when its surface is really mapped (the compositor configured it)"""
        t = ms()
        state = {'over': False}

        def result(ok, detail=None):
            if state['over']:
                return False
            state['over'] = True
            self.put(cid, ok, detail, ms() - t)

            def close():
                try:
                    stop()
                except Exception:
                    pass
                self.after(120, self.next_step)
            self.held(close)()          # show mode: the popup stays open a moment
            return False

        def surface_ok(*_):
            s = popover.get_surface()
            if s is not None and s.get_mapped():
                result(True, ok_detail)

        def on_map(*_):
            s = popover.get_surface()
            if s is None:
                return
            if s.get_mapped():
                result(True, ok_detail)
            else:
                s.connect('notify::mapped', surface_ok)

        popover.connect('map', on_map)
        popover.connect('closed', lambda *_: result(False, 'closed before mapping'))
        self.after(limit, lambda: result(False, 'not mapped'))
        start()

    def step_menu(self):
        pop = self.menubtn.get_popover()
        if pop is None:
            self.put('menu', False, 'no popover')
            GLib.idle_add(self.next_step)
            return
        self.wait_popup('menu', pop, self.menubtn.popup, self.menubtn.popdown)

    def step_combo(self):
        pop = find_child(self.combo, Gtk.Popover)
        if pop is None:
            self.put('combo', False, 'no popover')
            GLib.idle_add(self.next_step)
            return

        def start():
            if not self.combo.activate():       # GTK >= 4.18: DropDown's activate signal pops it up
                pop.popup()
        self.wait_popup('combo', pop, start, pop.popdown)

    def step_tooltip(self):
        # GTK 4 shows a tooltip only under a real pointer and has no synthetic pointer input. Its tooltip window is
        # a grab-less GdkPopup (autohide off), so the check maps one exactly like it next to the tooltip's widget.
        pop = Gtk.Popover(autohide=False, has_arrow=False, can_focus=False)
        pop.add_css_class('tooltip')
        pop.set_child(Gtk.Label(label='1'))
        pop.set_parent(self.tip_anchor)
        pop.set_position(Gtk.PositionType.BOTTOM)

        def stop():
            pop.popdown()
            pop.unparent()
        self.wait_popup('tooltip', pop, pop.popup, stop, ok_detail='grab-less popup')

    def step_text(self):
        # GTK 4 has no public key-event synthesis: the text goes in through GtkText's own keybinding signal
        # (insert-at-cursor, what an input method commit and a key press end in), into the focused field
        t = ms()
        self.entry.grab_focus()
        text = find_child(self.entry, Gtk.Text)
        if text is None:
            self.put('text', False, 'no text widget', ms() - t)
            GLib.idle_add(self.next_step)
            return

        def check():
            got = self.entry.get_text()
            active = self.win.is_active()
            focus = text.has_focus()
            ok = got == SAMPLE and focus
            self.put('text', ok, 'synthetic=insert-at-cursor focus=%s active=%s match=%s' % (
                yesno(focus), yesno(active), yesno(got == SAMPLE)), ms() - t)
            GLib.idle_add(self.held(self.next_step))
            return False

        def go():
            if SHOW:        # one character at a time, as a person types
                left = list(SAMPLE)

                def one():
                    text.emit('insert-at-cursor', left.pop(0))
                    if left:
                        return True
                    GLib.idle_add(check)
                    return False
                GLib.timeout_add(60, one)
                return False
            text.emit('insert-at-cursor', SAMPLE)
            GLib.idle_add(check)
            return False

        # wait (a little) for the compositor's keyboard focus, then type
        def wait_active(n=[0]):
            n[0] += 1
            if self.win.is_active() or n[0] > 10:
                go()
                return False
            return True
        GLib.timeout_add(30, wait_active)

    def step_clipboard(self):
        # GTK reads its own clipboard locally, so a round trip through the compositor needs a second connection:
        # this one sets the text, the second (with its own small window, which takes the keyboard focus so the
        # compositor offers it the selection) reads it back
        t = ms()
        state = {'over': False, 'd2': None, 'w2': None}

        def result(ok, detail):
            if state['over']:
                return False
            state['over'] = True
            self.put('clipboard', ok, detail, ms() - t)

            def close():
                if state['w2'] is not None:
                    state['w2'].destroy()
                self.after(120, self.next_step)
            self.held(close)()          # show mode: the reading window stays a moment
            return False

        self.display.get_clipboard().set(SAMPLE)
        d2 = Gdk.Display.open(None)
        if d2 is None:
            self.put('clipboard', False, 'second connection refused', ms() - t)
            GLib.idle_add(self.next_step)
            return
        state['d2'] = d2
        w2 = Gtk.Window(display=d2)
        w2.set_title('toolkit-audit')
        w2.set_default_size(120, 60)
        state['w2'] = w2
        cb2 = d2.get_clipboard()
        tries = {'n': 0}

        def on_read(cb, res):
            try:
                got = cb.read_text_finish(res)
            except GLib.Error:
                got = None
            if got == SAMPLE:
                result(True, 'two connections')
            elif tries['n'] < 6:
                tries['n'] += 1
                self.after(100, read)
            else:
                result(False, 'no offer' if got is None else 'different text')

        def read():
            if not state['over']:
                cb2.read_text_async(None, on_read)
            return False

        w2.connect('map', lambda *_: self.after(80, read))
        w2.present()
        self.after(1300, lambda: result(False, 'timeout'))

    def step_dnd(self):
        # a drag needs a pointer button held down (its serial); GTK 4 cannot fake one
        self.put('dnd', None, 'skipped: needs a held pointer button, GTK 4 has no synthetic pointer input')
        GLib.idle_add(self.held(self.next_step, HOLD_MS // 2))

    def step_fullscreen(self):
        w = self.win
        t = ms()
        state = {'phase': 'on', 'over': False, 'full': None}

        def result(ok, detail):
            if state['over']:
                return False
            state['over'] = True
            self.put('fullscreen', ok, detail, ms() - t)
            GLib.idle_add(self.held(self.next_step))
            return False

        def poll():
            if state['over']:
                return False
            if state['phase'] == 'on':
                fw, fh = win_size(w)
                if w.is_fullscreen() and (fw > REQ_W or fh > REQ_H):
                    state['full'] = '%dx%d' % (fw, fh)
                    state['phase'] = 'hold' if SHOW else 'off'
                    if SHOW:        # show mode: fullscreen stays on screen a moment
                        def leave():
                            state['phase'] = 'off'
                            w.unfullscreen()
                            return False
                        GLib.timeout_add(HOLD_MS, leave)
                    else:
                        w.unfullscreen()
                return True
            if state['phase'] == 'hold':
                return True
            if not w.is_fullscreen() and win_size(w) == (REQ_W, REQ_H):
                result(True, 'on=%s off=%dx%d' % (state['full'], REQ_W, REQ_H))
                return False
            return True

        def give_up():
            if state['phase'] == 'on':
                result(False, 'never fullscreen (%dx%d)' % win_size(w))
            else:
                result(False, 'on=%s, did not come back (%dx%d)' % ((state['full'],) + win_size(w)))
            return False

        w.fullscreen()
        GLib.timeout_add(25, poll)
        self.after(1400 + HOLD_MS, give_up)

    def run(self):
        self.steps = [self.step_static, self.step_theme, self.step_menu, self.step_combo, self.step_tooltip,
                      self.step_text, self.step_fullscreen, self.step_clipboard, self.step_dnd]
        GLib.timeout_add(max(1, DEADLINE_MS - ms()), self.on_deadline)
        self.win.present()
        self.loop.run()
        self.win.destroy()


def win_size(w):
    # the window's border box: what set_default_size asked for (the titlebar and a solid-csd border included, the
    # csd shadow not); get_width() alone leaves out the border
    ok, r = w.compute_bounds(w)
    if ok:
        return int(round(r.get_width())), int(round(r.get_height()))
    return w.get_width(), w.get_height()


def yesno(b):
    return 'yes' if b else 'no'


def find_child(widget, cls):
    c = widget.get_first_child()
    while c is not None:
        if isinstance(c, cls):
            return c
        f = find_child(c, cls)
        if f is not None:
            return f
        c = c.get_next_sibling()
    return None


def main():
    # PyGObject already tried to open the display when it imported Gtk; without one, no window can be made
    try:
        ok = Gtk.init_check() and Gdk.Display.get_default() is not None
        audit = Audit() if ok else None
    except RuntimeError:
        audit = None
    if audit is None:
        emit(VERSION, None, {i: {'ok': False, 'detail': 'no display'} for i in IDS}, 2)
        return 2
    audit.run()
    return 0


if __name__ == '__main__':
    sys.exit(main())
