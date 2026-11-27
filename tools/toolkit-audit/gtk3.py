#!/usr/bin/python3
# gtk3.py — the GTK 3 harness of `lordos-testbench --toolkits` (installed as /usr/lib/lordos/toolkit-audit/gtk3.py).
#
# A small GTK 3 window (PyGObject, Gtk 3.0) that checks from the inside how GTK 3 behaves on the running compositor,
# on the backend the orchestrator picks with GDK_BACKEND (wayland, or x11 under Xwayland). It prints ONE JSON line
# on stdout and exits by itself:
#
#   {"toolkit":"gtk3","version":"3.24.49","backend":"wayland",
#    "checks":{"map":{"ok":true,"ms":212}, ..., "dnd":{"ok":null,"detail":"skipped: ..."}}}
#
# The check ids are the same in every harness: map, size, scale, theme, backend, decoration, first_frame, menu,
# combo, tooltip, clipboard, dnd, text, fullscreen. "ok" is true, false, or null when the check could not be done
# (its "detail" then starts with "skipped:"). "ms" is the time the check took (first_frame and map: since the
# process started).
#
# Rules (the brief of `lordos-testbench --toolkits`):
#   - LordOS doesn't depend on GTK or PyGObject. Without them this prints the line with "installed": false and every
#     check skipped, and exits 3. It never installs anything. Without a display it exits 4 ("error": "no-display").
#   - No test hooks: everything is done with the toolkit's own API and its own synthetic events (key presses, a
#     button press that starts a drag), the way an application could. The orchestrator reads the Wayland globals
#     bound from this process's WAYLAND_DEBUG output on stderr.
#   - Needs only python3-gi and GTK 3's typelib (gir1.2-gtk-3.0); not python3-gi-cairo (nothing is drawn by hand).
#   - The result holds only the toolkit's name and version, the backend, numbers and check ids: no user name, path,
#     theme name or window title.
#   - It finishes within BUDGET seconds (a watchdog prints what it has and exits); the orchestrator kills it at 8 s.
#   - The window shows no words (icons, digits and a light or dark swatch), so it needs no translations.
#   - Show mode (--show, or LORDOS_TOOLKIT_AUDIT_SHOW=1; the test bench's Frameworks screen): the same checks and the
#     same JSON line, slowed down so a person can follow them (each popup held open about 0.9 s, the text typed one
#     character at a time, fullscreen held on and off), the current step's id in a label on the window (a check
#     id, never translated), and before the JSON line one progress line per step and per check on stdout:
#     "#lordos-ta step <id>" and "#lordos-ta check <id> pass|fail|skip". The budget is then 14.6 s (killed at 20 s).

import os
import sys
import json
import time
import threading


def _process_start():
    """time.monotonic() at the moment this process was started (exec), so first_frame and map include Python's
    and the bindings' start-up; the import time if /proc can't say."""
    now = time.monotonic()
    try:
        with open('/proc/self/stat') as f:
            st = f.read()
        ticks = int(st[st.rindex(')') + 2:].split()[19])      # field 22, starttime, in clock ticks since boot
        age = time.clock_gettime(time.CLOCK_BOOTTIME) - ticks / os.sysconf('SC_CLK_TCK')
        return now - age if 0 <= age < 60 else now
    except (OSError, ValueError, IndexError):
        return now


T0 = _process_start()
SHOW = '--show' in sys.argv[1:] or os.environ.get('LORDOS_TOOLKIT_AUDIT_SHOW') == '1'
BUDGET = 14.6 if SHOW else 4.6   # seconds from the start: after this, the remaining checks are skipped and the line printed
HOLD_MS = 900 if SHOW else 0     # show mode: how long a step stays on screen once decided
REQ_W, REQ_H = 600, 400
CHECK_IDS = ('map', 'size', 'scale', 'theme', 'backend', 'decoration', 'first_frame', 'menu', 'combo', 'tooltip',
             'clipboard', 'dnd', 'text', 'fullscreen')
TOKEN = 'lordos-ta-7319'   # the clipboard and drag-and-drop payload, and the text typed into the entry

result = {'toolkit': 'gtk3', 'version': None, 'backend': None, 'checks': {}}
_printed = threading.Lock()
_done = [False]


def ms_since(t):
    return int(round((time.monotonic() - t) * 1000))


def progress(line):
    """Show mode: one progress line on stdout (never after the JSON line)."""
    if not SHOW:
        return
    with _printed:
        if not _done[0]:
            sys.stdout.write('#lordos-ta ' + line + '\n')
            sys.stdout.flush()


def check(cid, ok, ms=None, detail=None):
    if cid in result['checks']:
        return
    c = {'ok': ok}
    if ms is not None:
        c['ms'] = ms
    if detail:
        c['detail'] = detail
    result['checks'][cid] = c
    if cid in CHECK_IDS:
        progress('check %s %s' % (cid, 'skip' if ok is None else 'pass' if ok is True else 'fail'))


def skip(cid, why):
    check(cid, None, detail='skipped: ' + why)


def emit(code=0):
    """Print the one JSON line (once) and exit."""
    with _printed:
        if _done[0]:
            return
        _done[0] = True
        got = dict(result['checks'])      # one C-level copy: the other thread may still be adding to it
        out = dict(result)
        out['checks'] = {k: got.get(k) or {'ok': None, 'detail': 'skipped: time budget'} for k in CHECK_IDS}
        sys.stdout.write(json.dumps(out, separators=(',', ':')) + '\n')
        sys.stdout.flush()
    os._exit(code)


def bail(error, code, why):
    result['error'] = error
    for cid in CHECK_IDS:
        skip(cid, why)
    emit(code)


def _timeout():
    # a window that never mapped (a compositor that doesn't answer) fails map and first_frame, not "skipped"
    for cid in ('map', 'first_frame'):
        check(cid, False, detail='none within %.1f s' % BUDGET)
    emit(0)


# last resort: a main loop stuck in a toolkit call (even the import, which connects) still prints within the budget
_wd = threading.Timer(BUDGET + 0.3, _timeout)
_wd.daemon = True
_wd.start()

# No accessibility bus: it is not what is measured and it costs a D-Bus round trip at start.
os.environ.setdefault('NO_AT_BRIDGE', '1')

try:
    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('Gdk', '3.0')
    from gi.repository import Gtk, Gdk, GLib, Gio
except (ImportError, ValueError):
    result['installed'] = False
    bail('not-installed', 3, 'not installed')

result['version'] = '%d.%d.%d' % (Gtk.get_major_version(), Gtk.get_minor_version(), Gtk.get_micro_version())

ok_init, _ = Gtk.init_check(sys.argv[:1])
if not ok_init or Gdk.Display.get_default() is None:
    bail('no-display', 4, 'no display')

DISPLAY = Gdk.Display.get_default()
_tn = DISPLAY.__gtype__.name          # GdkWaylandDisplay, GdkX11Display, ...
BACKEND = 'wayland' if 'Wayland' in _tn else 'x11' if 'X11' in _tn else _tn.replace('Gdk', '').replace('Display', '').lower()
result['backend'] = BACKEND
SEAT = DISPLAY.get_default_seat()


# ---------------------------------------------------------------- the window

class Harness:
    def __init__(self):
        self.first_draw = None
        self.mapped = False
        self.dnd_got = None
        self.steps = []

        w = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
        w.set_title('toolkit-audit gtk3')
        w.set_default_size(REQ_W, REQ_H)
        hb = Gtk.HeaderBar()
        hb.set_show_close_button(True)
        hb.set_title('toolkit-audit gtk3')
        w.set_titlebar(hb)
        self.win = w

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        box.set_border_width(12)
        w.add(box)
        # show mode: the current step's id (a fixed check id, not translated)
        self.step_label = None
        if SHOW:
            self.step_label = Gtk.Label()
            self.step_label.set_halign(Gtk.Align.START)
            box.pack_end(self.step_label, False, False, 0)

        # the top row: buttons strictly at the start and at the end
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.b_start = Gtk.Button.new_from_icon_name('go-previous-symbolic', Gtk.IconSize.BUTTON)
        self.b_end = Gtk.Button.new_from_icon_name('go-next-symbolic', Gtk.IconSize.BUTTON)
        row.pack_start(self.b_start, False, False, 0)
        row.pack_end(self.b_end, False, False, 0)
        # the menu: a MenuButton with a GtkMenu (an xdg_popup on Wayland, an override-redirect window on X11)
        self.menu = Gtk.Menu()
        for n in ('1', '2', '3'):
            self.menu.append(Gtk.MenuItem(label=n))
        self.menu.show_all()
        self.mbtn = Gtk.MenuButton()
        self.mbtn.set_image(Gtk.Image.new_from_icon_name('open-menu-symbolic', Gtk.IconSize.BUTTON))
        self.mbtn.set_popup(self.menu)
        row.pack_end(self.mbtn, False, False, 0)
        box.pack_start(row, False, False, 0)

        # the tooltip lives on the start button: a custom tooltip window, so its map can be seen
        self.tip = Gtk.Window(type=Gtk.WindowType.POPUP)
        self.tip.set_type_hint(Gdk.WindowTypeHint.TOOLTIP)
        self.tip.add(Gtk.Label(label='i'))
        self.tip.get_child().show()
        self.b_start.set_tooltip_window(self.tip)
        self.b_start.set_has_tooltip(True)
        self.b_start.connect('query-tooltip', lambda *a: True)

        self.entry = Gtk.Entry()
        box.pack_start(self.entry, False, False, 0)

        line = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        self.check_btn = Gtk.CheckButton()
        line.pack_start(self.check_btn, False, False, 0)
        self.scale_w = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 1)
        self.scale_w.set_value(40)
        line.pack_start(self.scale_w, True, True, 0)
        self.combo = Gtk.ComboBoxText()
        for n in ('1', '2', '3'):
            self.combo.append_text(n)
        self.combo.set_active(0)
        line.pack_start(self.combo, False, False, 0)
        box.pack_start(line, False, False, 0)

        # light or dark indicator: a swatch in the scheme that was read (CSS, so no cairo bindings are needed:
        # python3-gi-cairo is often missing)
        self.dark = None
        self.swatch = Gtk.Box()
        self.swatch.set_name('ta-swatch')
        self.swatch.set_size_request(48, 48)
        line2 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        line2.pack_start(self.swatch, False, False, 0)
        box.pack_start(line2, False, False, 0)

        # drag and drop within itself: the end button is the source, the entry the target
        self.dnd_target = Gtk.TargetEntry.new('text/plain', Gtk.TargetFlags.SAME_APP, 0)
        self.b_end.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, [self.dnd_target], Gdk.DragAction.COPY)
        self.b_end.connect('drag-data-get', self.on_drag_get)
        self.b_end.connect('drag-begin', self.on_drag_begin)
        self.dnd_began = None
        self.swatch.drag_dest_set(Gtk.DestDefaults.ALL, [self.dnd_target], Gdk.DragAction.COPY)
        self.swatch.connect('drag-data-received', self.on_drag_received)

        self.mark('map')
        w.connect('realize', self.on_realize)
        w.connect('map-event', self.on_map)
        w.connect('delete-event', lambda *a: emit(0))
        self.theme_read()
        css = Gtk.CssProvider()
        v = '#1f1f1f' if self.dark else '#ebebeb'
        css.load_from_data(('#ta-swatch { background-color: %s; border: 1px solid #808080; }' % v).encode())
        self.swatch.get_style_context().add_provider(css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        w.show_all()

    # -------------------------------------------------- show mode
    def mark(self, cid):
        """Show mode: the step now on screen, in the label and as a progress line."""
        if not SHOW:
            return
        progress('step ' + cid)
        if self.step_label is not None:
            self.step_label.set_markup('<span font_family="monospace" weight="bold" size="large">%s</span>' % cid)

    def held(self, fn):
        """fn, run HOLD_MS later in show mode (the step stays on screen), at once otherwise."""
        if not SHOW:
            return fn

        def later():
            GLib.timeout_add(max(1, min(HOLD_MS, int((BUDGET - (time.monotonic() - T0)) * 1000))), lambda: fn() and False)
        return later

    # -------------------------------------------------- first frame, map, then the static checks
    def on_realize(self, *_a):
        fc = self.win.get_window().get_frame_clock() if self.win.get_window() else None
        if fc is not None:
            fc.connect('after-paint', self.on_painted)

    def on_painted(self, *_a):
        if self.first_draw is None:
            self.first_draw = time.monotonic()
            check('first_frame', True, ms_since(T0), 'since start')

    def on_map(self, *_a):
        if self.mapped:
            return False
        self.mapped = True
        check('map', True, ms_since(T0), 'since start')
        GLib.timeout_add(150, self.after_map)
        return False

    def after_map(self):
        if self.first_draw is None:
            check('first_frame', False, detail='no frame painted by the map')
        self.mark('size')
        # the size the window settles at: read again every 30 ms for at most 600 ms after the map. Under XWayland a
        # ConfigureNotify can come a frame after the 150 ms above (the X server and the window manager answer in turn),
        # and one read then failed the check once in two runs on a real desktop (BUG-2 of the 0.8.9.6.2 QA)
        self.size_t = time.monotonic()
        GLib.timeout_add(1, self.size_settle)
        return False

    def size_settle(self):
        w, h = self.win.get_size()
        if (w, h) != (REQ_W, REQ_H) and time.monotonic() - self.size_t < 0.6:
            GLib.timeout_add(30, self.size_settle)
            return False
        check('size', (w, h) == (REQ_W, REQ_H), detail='%dx%d' % (w, h) +
              ('' if time.monotonic() - self.size_t < 0.05 else ' after %d ms' % ms_since(self.size_t)))
        f = self.win.get_scale_factor()
        mon = DISPLAY.get_monitor_at_window(self.win.get_window()) if self.win.get_window() else None
        mf = mon.get_scale_factor() if mon else None
        check('scale', f >= 1 and (mf is None or mf == f),
              detail='factor=%d' % f + ('' if mf is None else ' monitor=%d' % mf))
        req = (os.environ.get('GDK_BACKEND') or '').split(',')[0].strip()
        check('backend', (not req or req == '*' or req == BACKEND), detail=BACKEND + (' requested=' + req if req else ''))
        ctx = self.win.get_style_context()
        deco = 'solid-csd' if ctx.has_class('solid-csd') else 'csd' if ctx.has_class('csd') else 'ssd'
        check('decoration', bool(self.win.get_decorated()), detail=deco)
        self.steps = [self.step_menu, self.step_combo, self.step_tooltip, self.step_clipboard, self.step_text,
                      self.step_dnd, self.step_fullscreen]
        self.held(self.next_step)()
        return False

    # -------------------------------------------------- the steps, one after another, each with its own deadline
    def next_step(self):
        if time.monotonic() - T0 > BUDGET:
            emit(0)
            return False
        if not self.steps:
            GLib.idle_add(lambda: emit(0))
            return False
        self.steps.pop(0)()
        return False

    def later(self, ms, fn):
        """Run fn after ms, but never past the budget (then skip to the end)."""
        left = int((BUDGET - (time.monotonic() - T0)) * 1000)
        GLib.timeout_add(max(1, min(ms, left)), fn)

    def wait_for(self, cid, cond, timeout_ms, fail_detail, done=None, t=None, poll=20):
        """Poll cond() every `poll` ms until true or timeout; record the check, then call done() or next_step()."""
        t = t or time.monotonic()

        def tick():
            try:
                v = cond()
            except ValueError as e:  # the check's own failure, with its reason
                check(cid, False, ms_since(t), str(e))
                (done or self.next_step)()
                return False
            except Exception as e:  # a toolkit error is a failed check, not a crash
                check(cid, False, ms_since(t), 'error: ' + type(e).__name__)
                (done or self.next_step)()
                return False
            if v:
                check(cid, True, ms_since(t), v if isinstance(v, str) else None)
                (done or self.next_step)()
                return False
            if ms_since(t) >= timeout_ms or time.monotonic() - T0 > BUDGET:
                check(cid, False, ms_since(t), fail_detail() if callable(fail_detail) else fail_detail)
                (done or self.next_step)()
                return False
            return True
        GLib.timeout_add(poll, tick)

    @staticmethod
    def stays_mapped(win, hold_ms=120):
        """A condition: the window is mapped and has stayed mapped for hold_ms (a compositor that dismisses a
        popup, xdg_popup.popup_done, makes GTK hide it again)."""
        since = [None]

        def cond():
            gw = win.get_window()
            if win.get_mapped() and gw is not None and gw.is_viewable():
                if since[0] is None:
                    since[0] = time.monotonic()
                return ms_since(since[0]) >= hold_ms
            if since[0] is not None:
                raise ValueError('mapped, then closed')
            return False
        return cond

    # menu: popped up from its button with no trigger event (GTK uses the last one it has)
    def step_menu(self):
        self.mark('menu')
        t = time.monotonic()
        top = self.menu.get_toplevel()
        self.mbtn.set_active(True)
        mapped = self.stays_mapped(top)

        def close():
            self.mbtn.set_active(False)
            self.menu.popdown()
            self.later(60, self.next_step)
        self.wait_for('menu', mapped, 700, 'popup not mapped', self.held(close), t)

    # combo: its popup (a GtkMenu by default)
    def step_combo(self):
        self.mark('combo')
        t = time.monotonic()
        self.combo.popup()

        held = [None]

        def shown():
            if self.combo.get_property('popup-shown'):
                if held[0] is None:
                    held[0] = time.monotonic()
                return ms_since(held[0]) >= 120
            if held[0] is not None:
                raise ValueError('shown, then closed')
            return False

        def close():
            self.combo.popdown()
            self.later(60, self.next_step)
        self.wait_for('combo', shown, 700, 'popup not shown', self.held(close), t)

    # tooltip: GTK 3 shows a tooltip only where the real pointer is (it asks the device, so a synthetic motion
    # isn't enough and no test hook is used); the tooltip window is shown the way GTK shows one instead: a TOOLTIP
    # popup transient for the window, placed against the start button with move_to_rect (an xdg_popup without a
    # grab on Wayland, an override-redirect window on X11)
    def step_tooltip(self):
        self.mark('tooltip')
        t = time.monotonic()
        if self.win.get_window() is None:
            skip('tooltip', 'no window')
            self.next_step()
            return
        try:
            alloc = self.b_start.get_allocation()
            bx, by = self.b_start.translate_coordinates(self.win, 0, 0) or (0, 0)
            self.tip.set_transient_for(self.win)
            self.tip.realize()
            r = Gdk.Rectangle()
            r.x, r.y, r.width, r.height = bx, by, alloc.width, alloc.height
            self.tip.get_window().move_to_rect(r, Gdk.Gravity.SOUTH, Gdk.Gravity.NORTH,
                                               Gdk.AnchorHints.FLIP_Y | Gdk.AnchorHints.SLIDE_X, 0, 4)
            self.tip.show()
        except Exception as e:
            check('tooltip', False, ms_since(t), 'error: ' + type(e).__name__)
            self.next_step()
            return

        def close():
            self.tip.hide()
            self.later(30, self.next_step)
        self.wait_for('tooltip', self.stays_mapped(self.tip), 700, 'not shown', self.held(close), t)

    # clipboard: set a text and read it back, both through the toolkit; the compositor (Wayland) or the X server
    # must also confirm the new owner (GTK's owner-change), since GTK reads its own selection without a round trip
    def step_clipboard(self):
        self.mark('clipboard')
        t = time.monotonic()
        cb = Gtk.Clipboard.get_for_display(DISPLAY, Gdk.SELECTION_CLIPBOARD)
        got = [None]
        owner = [False]
        hid = cb.connect('owner-change', lambda *_a: owner.__setitem__(0, True))
        try:
            cb.set_text(TOKEN, -1)
            cb.request_text(lambda _c, txt: got.__setitem__(0, txt if txt is not None else ''))
        except Exception as e:
            cb.disconnect(hid)
            check('clipboard', False, ms_since(t), 'error: ' + type(e).__name__)
            self.next_step()
            return

        def back():
            if got[0] is None or not owner[0]:
                return False
            if got[0] != TOKEN:
                raise ValueError('another text came back')
            return True

        def fin():
            cb.disconnect(hid)
            c = result['checks'].get('clipboard')
            if c is not None and not c['ok'] and got[0] == TOKEN and not owner[0]:
                c['detail'] = 'text back, but the new owner was never confirmed'
            self.held(self.next_step)()
        self.wait_for('clipboard', back, 600, 'no text back', fin, t)

    # text: key presses made with the toolkit's own events, delivered to the window; the entry must hold the text
    def step_text(self):
        self.mark('text')
        t = time.monotonic()
        gw = self.win.get_window()
        kb = SEAT.get_keyboard() if SEAT else None
        if gw is None or kb is None:
            skip('text', 'no keyboard device')
            self.next_step()
            return
        self.win.present()
        self.entry.grab_focus()
        self.entry.set_text('')
        keymap = Gdk.Keymap.get_for_display(DISPLAY)

        def type_char(ch):
            kv = Gdk.unicode_to_keyval(ord(ch))
            code = 0
            okk, keys = keymap.get_entries_for_keyval(kv)
            if okk and keys:
                code = keys[0].keycode
            for typ in (Gdk.EventType.KEY_PRESS, Gdk.EventType.KEY_RELEASE):
                ev = Gdk.Event.new(typ)
                ev.window = gw
                ev.send_event = 1
                ev.time = Gtk.get_current_event_time()
                ev.keyval = kv
                ev.hardware_keycode = code
                ev.set_device(kb)
                ev.set_source_device(kb)
                Gtk.main_do_event(ev)

        def failed(e):
            check('text', False, ms_since(t), 'error: ' + type(e).__name__)
            self.next_step()

        def verify():
            self.wait_for('text', lambda: self.entry.get_text() == TOKEN, 300,
                          lambda: 'entry has %d of %d characters' % (len(self.entry.get_text()), len(TOKEN)),
                          self.held(self.next_step), t)

        if SHOW:            # one character at a time, as a person types
            left = list(TOKEN)

            def one():
                try:
                    type_char(left.pop(0))
                except Exception as e:
                    failed(e)
                    return False
                if left:
                    return True
                verify()
                return False
            GLib.timeout_add(60, one)
            return
        try:
            for ch in TOKEN:
                type_char(ch)
        except Exception as e:
            failed(e)
            return
        verify()

    # drag and drop: the toolkit starts a drag from the end button to the swatch with its own button press; the
    # drop needs the compositor (Wayland) or the X server to follow a pointer, so a drag that starts but gets no
    # drop is reported as skipped, not failed
    def step_dnd(self):
        self.mark('dnd')
        t = time.monotonic()
        gw = self.win.get_window()
        dev = SEAT.get_pointer() if SEAT else None
        if gw is None or dev is None:
            skip('dnd', 'no pointer device')
            self.next_step()
            return
        # GTK starts the drag from the window under the pointer; with the pointer elsewhere (or on no window yet)
        # GTK 3 on Wayland sends start_drag with no origin and libwayland ends the connection, so don't try
        under = dev.get_window_at_position()[0]
        if under is None or under.get_toplevel() != gw:
            skip('dnd', 'needs the pointer over the window')
            self.next_step()
            return
        alloc = self.b_end.get_allocation()
        x, y = self.b_end.translate_coordinates(self.win, alloc.width // 2, alloc.height // 2) or (0, 0)
        try:
            ev = Gdk.Event.new(Gdk.EventType.BUTTON_PRESS)
            ev.window = gw
            ev.send_event = 1
            ev.time = Gtk.get_current_event_time()
            ev.button = 1
            ev.x, ev.y = float(x), float(y)
            ev.x_root, ev.y_root = float(x), float(y)
            ev.set_device(dev)
            ev.set_source_device(dev)
            tl = Gtk.TargetList.new([self.dnd_target])
            ctx = self.b_end.drag_begin_with_coordinates(tl, Gdk.DragAction.COPY, 1, ev, x, y)
        except Exception as e:
            check('dnd', False, ms_since(t), 'error: ' + type(e).__name__)
            self.next_step()
            return
        self.dnd_ctx = ctx

        def wait():
            if self.dnd_got is not None:
                check('dnd', self.dnd_got == TOKEN, ms_since(t), None if self.dnd_got == TOKEN else 'wrong data')
                return fin()
            if ms_since(t) >= 700 or time.monotonic() - T0 > BUDGET:
                if ctx is None or self.dnd_began is None:
                    check('dnd', False, ms_since(t), 'drag did not start')
                else:
                    skip('dnd', 'drag started, the drop needs a real pointer')
                return fin()
            return True

        def fin():
            try:
                if ctx is not None:
                    Gtk.drag_cancel(ctx)
            except Exception:
                pass
            self.later(60 + HOLD_MS, self.next_step)
            return False
        GLib.timeout_add(20, wait)

    def on_drag_begin(self, *_a):
        self.dnd_began = time.monotonic()

    def on_drag_get(self, _w, _ctx, data, _info, _time):
        data.set_text(TOKEN, -1)

    def on_drag_received(self, _w, _ctx, _x, _y, data, _info, _time):
        self.dnd_got = data.get_text() or ''

    # fullscreen on, then off: the window-state the compositor confirms
    def step_fullscreen(self):
        self.mark('fullscreen')
        t = time.monotonic()
        st = self.win.get_window()
        if st is None:
            skip('fullscreen', 'no window')
            self.next_step()
            return
        FS = Gdk.WindowState.FULLSCREEN
        self.win.fullscreen()
        size_fs = []

        mon = DISPLAY.get_monitor_at_window(st)
        geo = mon.get_geometry() if mon else None
        seen = [None]

        def is_fs():
            # the state, then the size it settles at (GTK reports the state a moment before the new size)
            if not (st.get_state() & FS):
                return False
            if seen[0] is None:
                seen[0] = time.monotonic()
            w, h = self.win.get_size()
            if geo is not None and (w, h) != (geo.width, geo.height) and ms_since(seen[0]) < 300:
                return False
            size_fs.append((w, h))
            return True

        def off():
            if 'fullscreen' in result['checks']:   # "on" failed
                return self.next_step()
            t2 = time.monotonic()
            self.win.unfullscreen()

            def is_off():
                return not (st.get_state() & FS)

            def fin():
                c = result['checks'].pop('fullscreen_off')
                on_ms = result['checks'].pop('fullscreen_on')['ms']
                w, h = self.win.get_size()
                fw, fh = size_fs[0] if size_fs else (0, 0)
                fills = geo is None or (fw, fh) == (geo.width, geo.height)
                det = 'on %dx%d%s, off %dx%d' % (fw, fh, '' if geo is None else ' of %dx%d' % (geo.width, geo.height), w, h)
                check('fullscreen', bool(c['ok'] and fills), on_ms + c.get('ms', 0),
                      det if c['ok'] else 'did not leave fullscreen')
                self.held(self.next_step)()
            self.wait_for('fullscreen_off', is_off, 800, 'still fullscreen', fin, t2)

        def on_done():
            c = result['checks'].get('fullscreen_on')
            if not c or not c['ok']:
                result['checks'].pop('fullscreen_on', None)
                check('fullscreen', False, ms_since(t), 'not fullscreen')
                self.win.unfullscreen()
                return self.next_step()
            self.held(off)()             # show mode: fullscreen stays on screen a moment
        self.wait_for('fullscreen_on', is_fs, 900, 'not fullscreen', on_done, t)

    # -------------------------------------------------- light or dark
    def theme_read(self):
        """The color scheme GTK renders with (prefer-dark, or a theme whose name ends in -dark: what GTK 3 reads
        from GSettings on Wayland and from XSETTINGS on X11), compared with the desktop portal's Settings
        (org.freedesktop.appearance color-scheme) when a session bus has one: a GTK 3 window that stays light on a
        dark desktop fails."""
        t = time.monotonic()
        tk = None
        s = Gtk.Settings.get_default()
        if s is not None:
            name = (s.get_property('gtk-theme-name') or '').lower()
            tk = bool(s.get_property('gtk-application-prefer-dark-theme')) or name.endswith('-dark') or name.endswith(':dark')
        portal = None
        bus_ok = bool(os.environ.get('DBUS_SESSION_BUS_ADDRESS')) or \
            os.path.exists(os.path.join(os.environ.get('XDG_RUNTIME_DIR') or '/nonexistent', 'bus'))
        if bus_ok:   # never autolaunch a bus
            try:
                bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
                v = bus.call_sync('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                                  'org.freedesktop.portal.Settings', 'ReadOne',
                                  GLib.Variant('(ss)', ('org.freedesktop.appearance', 'color-scheme')),
                                  None, Gio.DBusCallFlags.NO_AUTO_START, 400, None).unpack()[0]
                if isinstance(v, int):
                    portal = v      # 0 no preference, 1 dark, 2 light
            except Exception:
                pass
        self.dark = tk
        word = lambda d: 'dark' if d else 'light'
        if tk is None:
            check('theme', False, ms_since(t), 'no GTK settings')
        elif portal in (1, 2):
            check('theme', tk == (portal == 1), ms_since(t), 'toolkit %s, portal %s' % (word(tk), word(portal == 1)))
        else:
            check('theme', True, ms_since(t), 'toolkit %s, %s' % (word(tk), 'portal no preference' if portal == 0 else 'no portal'))


def main():
    Harness()

    def no_map():
        _timeout()
        return False
    GLib.timeout_add(int(BUDGET * 1000), no_map)
    Gtk.main()
    emit(0)


if __name__ == '__main__':
    main()
