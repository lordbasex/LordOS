#!/usr/bin/python3
# tools/toolkit-audit/qt5.py — the Qt 5 harness of the toolkit audit (`lordos-testbench --toolkits`, docs/WAYLAND.md).
# Installed as /usr/lib/lordos/toolkit-audit/qt5.py. It runs only when PyQt5 is already installed (python3-pyqt5,
# and qtwayland5 for the Wayland backend): LordOS depends on neither and this program never installs anything.
#
# The orchestrator picks the backend with QT_QPA_PLATFORM (wayland or xcb) and, on Wayland, sets WAYLAND_DEBUG=1 to
# read the globals bound from stderr. This program opens one 600x400 window (QMainWindow: a menu bar, a top row with
# one button at the start and one at the end, a light/dark indicator between them, a line edit, a check box, a slider
# and a combo box), runs the checks it can see from inside, prints ONE JSON line to stdout and exits:
#
#   {"toolkit":"qt5","version":"5.15.15","backend":"wayland","checks":{"map":{"ok":true,"ms":120},...}}
#
# Check ids (shared by every harness): map, size, scale, theme, backend, decoration, first_frame, menu, combo,
# tooltip, clipboard, dnd, text, fullscreen. Each is {"ok": true|false|null, "ms"?: n, "detail"?: str}; null is
# "skipped: why". It exits by itself within 5 s (a watchdog thread prints what it has at 4.6 s, even when Qt is
# stuck in a nested loop). Privacy: only the toolkit, its version, the backend, numbers and check ids leave it; the
# window title is fixed and no path, user or environment value is printed.
# Exit status: 0 the checks ran, 2 PyQt5 is not installed, 3 the asked backend can't start (no display, no platform
# plugin: qtwayland5 for Wayland, or the display refused the connection); the JSON line says so too.
import json
import os
import sys
import threading
import time

T0 = time.monotonic()
CHECK_IDS = ('map', 'size', 'scale', 'theme', 'backend', 'decoration', 'first_frame', 'menu', 'combo', 'tooltip',
             'clipboard', 'dnd', 'text', 'fullscreen')
WANT_W, WANT_H = 600, 400
DEADLINE_S = 4.6            # the orchestrator asks for an exit within 5 s and kills at 8 s

_lock = threading.Lock()
_done = False
result = {'toolkit': 'qt5', 'version': None, 'backend': None, 'checks': {}}


def emit_and_exit(code=0):
    """Prints the one JSON line once (from the main thread or the watchdog) and leaves without Qt's teardown."""
    global _done
    with _lock:
        if _done:
            return
        _done = True
        checks = result['checks']
        for cid in CHECK_IDS:
            if cid not in checks:
                checks[cid] = {'ok': False, 'detail': 'timed out'}
        out = {'toolkit': result['toolkit'], 'version': result['version'], 'backend': result['backend'],
               'checks': {cid: checks[cid] for cid in CHECK_IDS}}
        try:
            sys.stdout.write(json.dumps(out, separators=(',', ':')) + '\n')
            sys.stdout.flush()
        except Exception:
            pass
        os._exit(code)


def ms_since(t):
    return int(round((time.monotonic() - t) * 1000))


def process_age_ms():
    """Milliseconds since this process started (the kernel's start time), so the first frame counts Python and Qt
    loading too; falls back to the time since this file started running."""
    try:
        with open('/proc/self/stat', 'rb') as f:
            stat = f.read().decode('ascii', 'replace')
        start_ticks = int(stat[stat.rindex(')') + 2:].split()[19])
        hz = os.sysconf('SC_CLK_TCK')
        with open('/proc/uptime') as f:
            uptime = float(f.read().split()[0])
        age = uptime - start_ticks / hz
        if 0 <= age < 60:
            return int(round(age * 1000))
    except Exception:
        pass
    return ms_since(T0)


def check(cid, ok, ms=None, detail=None):
    if cid in result['checks']:
        return
    c = {'ok': ok}
    if ms is not None:
        c['ms'] = int(ms)
    if detail:
        c['detail'] = str(detail)[:200]
    result['checks'][cid] = c


def skip(cid, why):
    check(cid, None, detail='skipped: ' + why)


try:
    from PyQt5 import QtCore, QtGui, QtWidgets
except Exception:
    for cid in CHECK_IDS:
        skip(cid, 'PyQt5 is not installed')
    emit_and_exit(2)

result['version'] = QtCore.qVersion()

_watchdog = threading.Timer(max(0.5, DEADLINE_S - (time.monotonic() - T0)), emit_and_exit)
_watchdog.daemon = True
_watchdog.start()


def requested_backend():
    v = os.environ.get('QT_QPA_PLATFORM', '')
    for sep in (';', ':'):
        v = v.split(sep)[0]
    return v.strip() or None


def norm_backend(name):
    if name == 'xcb':
        return 'x11'
    if name and name.startswith('wayland'):
        return 'wayland'
    return name or 'unknown'


class Win(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.first_paint_ms = None
        self.dropped = False
        self.setWindowTitle('lordos-toolkit-audit')
        self.setAcceptDrops(True)

        self.menu = self.menuBar().addMenu('☰')
        self.menu.addAction('1')
        self.menu.addAction('2')

        central = QtWidgets.QWidget(self)
        col = QtWidgets.QVBoxLayout(central)
        top = QtWidgets.QHBoxLayout()
        self.start_btn = QtWidgets.QToolButton(central)
        self.start_btn.setText('◀')
        self.end_btn = QtWidgets.QToolButton(central)
        self.end_btn.setText('▶')
        self.indicator = QtWidgets.QLabel('◐', central)
        self.indicator.setAlignment(QtCore.Qt.AlignCenter)
        # strictly start- and end-aligned: Qt mirrors the row by itself in a right-to-left layout
        top.addWidget(self.start_btn, 0, QtCore.Qt.AlignLeading)
        top.addStretch(1)
        top.addWidget(self.indicator)
        top.addStretch(1)
        top.addWidget(self.end_btn, 0, QtCore.Qt.AlignTrailing)
        col.addLayout(top)

        self.entry = QtWidgets.QLineEdit(central)
        self.check = QtWidgets.QCheckBox(central)
        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal, central)
        self.slider.setRange(0, 100)
        self.slider.setValue(50)
        self.combo = QtWidgets.QComboBox(central)
        self.combo.addItems(['1', '2', '3'])
        for w in (self.entry, self.check, self.slider, self.combo):
            col.addWidget(w)
        col.addStretch(1)
        self.setCentralWidget(central)

    def paintEvent(self, ev):
        super().paintEvent(ev)
        if self.first_paint_ms is None:
            self.first_paint_ms = process_age_ms()

    def dragEnterEvent(self, ev):
        if ev.mimeData().hasFormat('application/x-lordos-audit'):
            ev.acceptProposedAction()

    def dropEvent(self, ev):
        if ev.mimeData().hasFormat('application/x-lordos-audit'):
            self.dropped = True
            ev.acceptProposedAction()

    def set_scheme(self, dark):
        self.indicator.setText('☾' if dark else '☀')


def exposed(w):
    h = w.windowHandle() if w is not None else None
    return bool(w is not None and w.isVisible() and h is not None and h.isExposed())


def read_portal_scheme():
    """org.freedesktop.appearance color-scheme from the portal's Settings (ReadOne, else the older Read):
    0 no preference, 1 dark, 2 light; None when nothing answered."""
    try:
        from PyQt5 import QtDBus
    except Exception:
        return None, 'QtDBus is not available'
    bus = QtDBus.QDBusConnection.sessionBus()
    if not bus.isConnected():
        return None, 'no session bus'
    iface = QtDBus.QDBusInterface('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                                  'org.freedesktop.portal.Settings', bus)
    iface.setTimeout(1000)
    for method in ('ReadOne', 'Read'):
        msg = iface.call(method, 'org.freedesktop.appearance', 'color-scheme')
        if msg.type() != QtDBus.QDBusMessage.ReplyMessage:
            continue
        args = msg.arguments()
        v = args[0] if args else None
        for _ in range(3):              # Read wraps the value in one more variant
            if isinstance(v, QtDBus.QDBusVariant):
                v = v.variant()
        try:
            return int(v), method
        except (TypeError, ValueError):
            continue
    return None, 'the portal did not answer'


class InputSeen(QtCore.QObject):
    """Notes whether any real (spontaneous) key or button press reached the app: QTest's and sendEvent's don't."""
    def __init__(self):
        super().__init__()
        self.real_input = False

    def eventFilter(self, obj, ev):
        if not self.real_input and ev.spontaneous() and ev.type() in (
                QtCore.QEvent.KeyPress, QtCore.QEvent.MouseButtonPress, QtCore.QEvent.TouchBegin):
            self.real_input = True
        return False


class Audit:
    def __init__(self, app, win):
        self.app = app
        self.win = win
        self.filter = InputSeen()
        app.installEventFilter(self.filter)
        self.focus_changed = time.monotonic()
        app.focusWindowChanged.connect(self.on_focus)
        self.steps = [self.s_map, self.s_size, self.s_scale, self.s_backend, self.s_decoration, self.s_theme,
                      self.s_text, self.s_clipboard, self.s_tooltip, self.s_menu, self.s_combo, self.s_fullscreen,
                      self.s_dnd]
        self.t_show = None

    def on_focus(self, _w):
        self.focus_changed = time.monotonic()

    # a tiny step runner: a step either finishes at once or waits with poll(), then calls next()
    def next(self):
        if not self.steps:
            QtCore.QTimer.singleShot(0, lambda: emit_and_exit(0))
            return
        step = self.steps.pop(0)
        QtCore.QTimer.singleShot(0, lambda: self.run(step))

    def run(self, step):
        try:
            step()
        except Exception as e:      # one broken check must not cost the others
            name = step.__name__[2:]
            check(name if name in CHECK_IDS else 'map', False, detail='error: ' + type(e).__name__)
            self.next()

    def poll(self, cond, timeout_ms, done):
        t = time.monotonic()
        timer = QtCore.QTimer(self.win)

        def tick():
            ok = False
            try:
                ok = bool(cond())
            except Exception:
                ok = False
            if ok or ms_since(t) >= timeout_ms:
                timer.stop()
                timer.deleteLater()
                done(ok, ms_since(t))
        timer.timeout.connect(tick)
        timer.start(10)
        tick()

    def start(self):
        self.t_show = time.monotonic()
        self.win.resize(WANT_W, WANT_H)
        self.win.show()
        self.next()

    # -- checks
    def s_map(self):
        def done(ok, _ms):
            check('map', ok, ms=ms_since(self.t_show) if ok else None,
                  detail=None if ok else 'the window was not exposed within 2 s')
            if not ok:
                skip('first_frame', 'the window never mapped')
                self.next()
                return
            # the first paint follows the expose; give it a moment
            self.poll(lambda: self.win.first_paint_ms is not None, 500, lambda ok2, _m: self.after_map(ok2))
        self.poll(lambda: exposed(self.win), 2000, done)

    def after_map(self, painted):
        if painted:
            check('first_frame', True, ms=self.win.first_paint_ms, detail='from process start')
        else:
            check('first_frame', False, detail='no paint after the window was exposed')
        self.next()

    def s_size(self):
        if not exposed(self.win):
            skip('size', 'the window never mapped')
            self.next()
            return
        last = [None]
        stable = [0]

        def settled():                  # the size the compositor configured, unchanged for 100 ms
            s = (self.win.width(), self.win.height())
            if s == last[0]:
                stable[0] += 1
            else:
                last[0], stable[0] = s, 0
            return s == (WANT_W, WANT_H) or stable[0] >= 10

        def done(_ok, _ms):
            w, h = self.win.width(), self.win.height()
            check('size', (w, h) == (WANT_W, WANT_H), detail='%dx%d (asked %dx%d)' % (w, h, WANT_W, WANT_H))
            self.next()
        self.poll(settled, 600, done)

    def s_scale(self):
        h = self.win.windowHandle()
        dpr = h.devicePixelRatio() if h is not None else self.win.devicePixelRatioF()
        screen = h.screen() if h is not None else None
        sdpr = screen.devicePixelRatio() if screen is not None else dpr
        check('scale', dpr >= 1.0 and abs(dpr - sdpr) < 0.01, detail='%g (screen %g)' % (dpr, sdpr))
        self.next()

    def s_backend(self):
        name = QtGui.QGuiApplication.platformName()
        result['backend'] = norm_backend(name)
        want = requested_backend()
        if want is None:
            check('backend', True, detail=name + ' (none requested)')
        else:
            check('backend', name == want, detail=name if name == want else '%s (asked %s)' % (name, want))
        self.next()

    def s_decoration(self):
        h = self.win.windowHandle()
        if h is None or not exposed(self.win):
            skip('decoration', 'the window never mapped')
            self.next()
            return
        m = h.frameMargins()
        margins = m.top() + m.left() + m.right() + m.bottom()
        name = QtGui.QGuiApplication.platformName()
        if name.startswith('wayland'):
            if margins > 0:
                check('decoration', True, detail='csd (top %d)' % m.top())
            elif os.environ.get('QT_WAYLAND_DISABLE_WINDOWDECORATION') == '1':
                check('decoration', False, detail='none (QT_WAYLAND_DISABLE_WINDOWDECORATION)')
            elif has_decoration_plugin():
                # Qt 5.15 hides its own frame only when the compositor asks for server-side decoration
                # (xdg-decoration); the Wayland log shows the mode
                check('decoration', True, detail='ssd')
            else:
                check('decoration', False, detail='none (no Qt decoration plugin and no frame)')
        else:
            # a window manager that frames a window sets _NET_FRAME_EXTENTS on it; LordOS's draws the frame outside
            # the X window and says 0 0 0 0, so the property being there is what tells
            if margins > 0:
                check('decoration', True, detail='ssd (top %d)' % m.top())
            else:
                ext = x11_frame_extents(int(self.win.winId()))
                if ext is None:
                    skip('decoration', 'the X property could not be read')
                elif ext is False:
                    check('decoration', False, detail='none (no _NET_FRAME_EXTENTS)')
                else:
                    check('decoration', True, detail='ssd (outside the X window, _NET_FRAME_EXTENTS %s)'
                          % ' '.join(str(n) for n in ext))
        self.next()

    def s_theme(self):
        pal_dark = self.app.palette().color(QtGui.QPalette.Window).lightness() < 128
        t = time.monotonic()
        scheme, how = read_portal_scheme()
        if scheme is None:
            self.win.set_scheme(pal_dark)
            if how == 'QtDBus is not available':
                skip('theme', how + '; palette ' + ('dark' if pal_dark else 'light'))
            else:
                check('theme', False, detail='%s; palette %s' % (how, 'dark' if pal_dark else 'light'))
        else:
            names = {0: 'default', 1: 'dark', 2: 'light'}
            dark = scheme == 1
            self.win.set_scheme(dark)
            check('theme', scheme in names, ms=ms_since(t),
                  detail='portal %s (%s); palette %s' % (names.get(scheme, str(scheme)), how,
                                                         'dark' if pal_dark else 'light'))
        self.next()

    def s_text(self):
        try:
            from PyQt5.QtTest import QTest
        except Exception:
            QTest = None
        self.win.activateWindow()
        self.win.entry.setFocus(QtCore.Qt.OtherFocusReason)
        word = 'lordos'

        def typed(_ok, _ms):
            t = time.monotonic()
            if QTest is not None:
                QTest.keyClicks(self.win.entry, word)
                how = 'QTest'
            else:
                for ch in word:
                    for kind in (QtCore.QEvent.KeyPress, QtCore.QEvent.KeyRelease):
                        ev = QtGui.QKeyEvent(kind, QtCore.Qt.Key_A + ord(ch) - ord('a'), QtCore.Qt.NoModifier, ch)
                        QtWidgets.QApplication.sendEvent(self.win.entry, ev)
                how = 'key events'
            got = self.win.entry.text()
            active = self.win.isActiveWindow()
            check('text', got == word, ms=ms_since(t),
                  detail='%s, %d of %d characters; keyboard focus %s' % (how, len(got) if got == word else
                                                                         sum(a == b for a, b in zip(got, word)),
                                                                         len(word), 'yes' if active else 'no'))
            self.next()
        # wait a little for the compositor's keyboard focus; type anyway after that
        self.poll(lambda: self.win.isActiveWindow(), 500, typed)

    def s_clipboard(self):
        cb = QtWidgets.QApplication.clipboard()
        token = 'lordos-audit-%d' % (int(time.monotonic() * 1e6) % 1000000007)
        t = time.monotonic()
        cb.setText(token, QtGui.QClipboard.Clipboard)

        def done(ok, _ms):
            owns = cb.ownsClipboard()
            check('clipboard', ok, ms=ms_since(t) if ok else None,
                  detail='own selection %s' % ('yes' if owns else 'no') if ok
                  else 'read back something else; own selection %s' % ('yes' if owns else 'no'))
            self.next()
        self.poll(lambda: cb.text(QtGui.QClipboard.Clipboard) == token, 1000, done)

    def settle(self, then):
        """Waits (up to 500 ms) until the main window has the keyboard again: a popup opened while the compositor
        is still giving the focus back after the last one is closed by Qt at once (a focus change closes popups)."""
        # focusWindow follows the compositor's keyboard focus (wl_keyboard.enter; isActiveWindow ignores popups).
        # The focus comes back a frame or so after the popup is gone, so it must also have been quiet for 100 ms.
        t = time.monotonic()

        def back():
            now = time.monotonic()
            return (now - t >= 0.1 and now - self.focus_changed >= 0.1
                    and QtGui.QGuiApplication.focusWindow() is self.win.windowHandle())
        self.poll(back, 700, lambda _ok, _ms: then())

    def popup_kind(self):
        """Qt 5 on Wayland makes a Qt::Popup an xdg_popup only after a real key or button press (it needs that
        input serial for the grab); before one, menus and combo lists open as toplevel windows."""
        if not QtGui.QGuiApplication.platformName().startswith('wayland'):
            return None
        return None if self.filter.real_input else 'as a toplevel window: Qt 5 opens an xdg_popup only after real input'

    def s_tooltip(self):
        btn = self.win.end_btn
        at = btn.mapToGlobal(QtCore.QPoint(4, btn.height()))

        def tip():
            for w in QtWidgets.QApplication.allWidgets():   # QTipLabel has the widget as its parent
                if w.inherits('QTipLabel') and w.isVisible():
                    return w
            return None

        def go():
            t = time.monotonic()
            QtWidgets.QToolTip.showText(at, '50', btn, QtCore.QRect(), 3000)

            def done(ok, _ms):
                QtWidgets.QToolTip.hideText()
                if ok:
                    check('tooltip', True, ms=ms_since(t), detail='QToolTip')
                    self.next()
                    return
                # QToolTip hides itself when the pointer isn't over its widget (a Leave or an activation change),
                # and there is no real pointer here: the same kind of window (Qt::ToolTip, an xdg_popup without a
                # grab on Wayland) is shown directly instead
                label = QtWidgets.QLabel('50', btn, QtCore.Qt.ToolTip)
                label.move(at)
                t2 = time.monotonic()
                label.show()

                def done2(ok2, _ms2):
                    check('tooltip', ok2, ms=ms_since(t2) if ok2 else None,
                          detail='a Qt::ToolTip window; QToolTip hid itself without a pointer over the button'
                          if ok2 else 'neither QToolTip nor a Qt::ToolTip window was exposed')
                    label.hide()
                    label.deleteLater()
                    self.settle(self.next)
                self.poll(lambda: exposed(label), 1000, done2)
            self.poll(lambda: exposed(tip()), 600, done)
        self.settle(go)

    def s_menu(self):
        menu = self.win.menu

        def go():
            t = time.monotonic()
            # as a click on the menu bar would: the bar opens the menu as a popup below its title
            self.win.menuBar().setActiveAction(menu.menuAction())
            if not menu.isVisible():
                menu.popup(self.win.menuBar().mapToGlobal(QtCore.QPoint(0, self.win.menuBar().height())))

            def done(ok, _ms):
                check('menu', ok, ms=ms_since(t) if ok else None,
                      detail=self.popup_kind() if ok else
                      ('shown but not exposed' if menu.isVisible() else 'closed at once'))
                menu.hide()
                self.win.menuBar().setActiveAction(None)
                self.settle(self.next)
            self.poll(lambda: exposed(menu), 1000, done)
        self.settle(go)

    def s_combo(self):
        combo = self.win.combo

        def popup():
            v = combo.view()
            return v.window() if v is not None else None

        def go():
            t = time.monotonic()
            combo.showPopup()

            def done(ok, _ms):
                p = popup()
                check('combo', ok, ms=ms_since(t) if ok else None,
                      detail=self.popup_kind() if ok else
                      ('shown but not exposed' if p is not None and p.isVisible() else 'closed at once'))
                combo.hidePopup()
                self.settle(self.next)
            self.poll(lambda: exposed(popup()), 1000, done)
        self.settle(go)

    def s_fullscreen(self):
        win = self.win
        if not exposed(win):
            skip('fullscreen', 'the window never mapped')
            self.next()
            return
        normal = (win.width(), win.height())
        h = win.windowHandle()
        screen = h.screen() if h is not None else None
        sg = screen.geometry() if screen is not None else None
        full_size = (sg.width(), sg.height()) if sg is not None else None
        t = time.monotonic()
        win.showFullScreen()

        def is_full():
            if not (win.windowState() & QtCore.Qt.WindowFullScreen):
                return False
            if full_size is not None:
                return (win.width(), win.height()) == full_size
            return (win.width(), win.height()) != normal

        def on_full(ok, _ms):
            full_ms = ms_since(t)
            got = (win.width(), win.height())
            if not ok:
                win.showNormal()
                check('fullscreen', False,
                      detail='on: %dx%d, not the screen %s' % (got[0], got[1],
                                                               '%dx%d' % full_size if full_size else '?'))
                self.poll(lambda: (win.width(), win.height()) == normal, 800, lambda _o, _m: self.next())
                return
            t2 = time.monotonic()
            win.showNormal()

            def on_normal(ok2, _ms2):
                back = (win.width(), win.height())
                check('fullscreen', ok2, ms=full_ms + ms_since(t2),
                      detail='on %dx%d in %d ms, off %dx%d' % (got[0], got[1], full_ms, back[0], back[1]))
                self.next()
            self.poll(lambda: not (win.windowState() & QtCore.Qt.WindowFullScreen)
                      and (win.width(), win.height()) == normal, 1200, on_normal)
        self.poll(is_full, 1200, on_full)

    def s_dnd(self):
        # a drag from the window's own button onto the window itself. Starting a drag needs the serial of a held
        # pointer button (Wayland) or a pointer grab (X11): without real input the compositor may refuse it, and the
        # drop needs the pointer to move, so with no drop this is a skip, not a failure. The drag is cancelled
        # after 600 ms whatever happens (QDrag.exec runs a nested event loop).
        if not exposed(self.win):
            skip('dnd', 'the window never mapped')
            self.next()
            return
        md = QtCore.QMimeData()
        md.setData('application/x-lordos-audit', QtCore.QByteArray(b'1'))
        drag = QtGui.QDrag(self.win.start_btn)
        drag.setMimeData(md)
        QtCore.QTimer.singleShot(600, QtGui.QDrag.cancel)
        t = time.monotonic()
        action = drag.exec_(QtCore.Qt.CopyAction)
        took = ms_since(t)
        if self.win.dropped:
            check('dnd', True, ms=took)
        elif took < 500:                # QDrag.exec came back before the cancel: Qt never started the drag
            skip('dnd', 'Qt starts no drag without a held pointer button (no real input here)')
        else:
            skip('dnd', 'the drag started and was cancelled; the drop needs real pointer motion (%s)' %
                 ('no action' if action == QtCore.Qt.IgnoreAction else 'an action but no drop'))
        self.next()


def x11_frame_extents(xid):
    """_NET_FRAME_EXTENTS of an X window as [left, right, top, bottom]; False when it isn't set, None when it can't
    be read. Read with libxcb (which Qt's xcb plugin links) over a connection of its own."""
    import ctypes
    import ctypes.util

    class Cookie(ctypes.Structure):
        _fields_ = [('sequence', ctypes.c_uint)]

    class AtomReply(ctypes.Structure):
        _fields_ = [('response_type', ctypes.c_uint8), ('pad0', ctypes.c_uint8), ('sequence', ctypes.c_uint16),
                    ('length', ctypes.c_uint32), ('atom', ctypes.c_uint32)]

    class PropReply(ctypes.Structure):
        _fields_ = [('response_type', ctypes.c_uint8), ('format', ctypes.c_uint8), ('sequence', ctypes.c_uint16),
                    ('length', ctypes.c_uint32), ('type', ctypes.c_uint32), ('bytes_after', ctypes.c_uint32),
                    ('value_len', ctypes.c_uint32), ('pad0', ctypes.c_uint8 * 12)]
    try:
        x = ctypes.CDLL(ctypes.util.find_library('xcb') or 'libxcb.so.1')
        libc = ctypes.CDLL(None)
    except OSError:
        return None
    x.xcb_connect.restype = ctypes.c_void_p
    x.xcb_connect.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    x.xcb_connection_has_error.argtypes = [ctypes.c_void_p]
    x.xcb_disconnect.argtypes = [ctypes.c_void_p]
    x.xcb_intern_atom.restype = Cookie
    x.xcb_intern_atom.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_char_p]
    x.xcb_intern_atom_reply.restype = ctypes.POINTER(AtomReply)
    x.xcb_intern_atom_reply.argtypes = [ctypes.c_void_p, Cookie, ctypes.c_void_p]
    x.xcb_get_property.restype = Cookie
    x.xcb_get_property.argtypes = [ctypes.c_void_p, ctypes.c_uint8] + [ctypes.c_uint32] * 5
    x.xcb_get_property_reply.restype = ctypes.POINTER(PropReply)
    x.xcb_get_property_reply.argtypes = [ctypes.c_void_p, Cookie, ctypes.c_void_p]
    x.xcb_get_property_value.restype = ctypes.POINTER(ctypes.c_uint32)
    x.xcb_get_property_value.argtypes = [ctypes.c_void_p]
    libc.free.argtypes = [ctypes.c_void_p]
    c = x.xcb_connect(None, None)
    if not c:
        return None
    try:
        if x.xcb_connection_has_error(c):
            return None
        name = b'_NET_FRAME_EXTENTS'
        ar = x.xcb_intern_atom_reply(c, x.xcb_intern_atom(c, 1, len(name), name), None)
        if not ar:
            return None
        atom = ar.contents.atom
        libc.free(ar)
        if atom == 0:                   # no client ever named it: not set on any window
            return False
        pr = x.xcb_get_property_reply(c, x.xcb_get_property(c, 0, xid, atom, 6, 0, 4), None)   # 6: CARDINAL
        if not pr:
            return None
        try:
            r = pr.contents
            if r.format != 32 or r.value_len < 4:
                return False
            v = x.xcb_get_property_value(pr)
            return [int(v[i]) for i in range(4)]
        finally:
            libc.free(pr)
    finally:
        x.xcb_disconnect(c)


def has_decoration_plugin():
    dirs = [QtCore.QLibraryInfo.location(QtCore.QLibraryInfo.PluginsPath)]
    dirs += [d for d in os.environ.get('QT_PLUGIN_PATH', '').split(':') if d]
    for d in dirs:
        try:
            if any(n.endswith('.so') for n in os.listdir(os.path.join(d, 'wayland-decoration-client'))):
                return True
        except OSError:
            pass
    return False


def platform_missing():
    """Why the asked backend can't start, or None. Qt aborts (qFatal, no exception) when it can't open the display
    or find the platform plugin, so both are looked at first."""
    v = os.environ.get('QT_QPA_PLATFORM', '')
    if ';' in v:                        # a fallback list: Qt picks the first that works
        return None
    want = requested_backend()
    if want == 'wayland' or (want and want.startswith('wayland')):
        if not os.environ.get('WAYLAND_DISPLAY') and not os.environ.get('WAYLAND_SOCKET'):
            return 'no Wayland display'
        name = 'libqwayland-'
    elif want == 'xcb':
        if not os.environ.get('DISPLAY'):
            return 'no X display'
        name = 'libqxcb'
    else:
        return None
    dirs = [QtCore.QLibraryInfo.location(QtCore.QLibraryInfo.PluginsPath)]
    dirs += [d for d in os.environ.get('QT_PLUGIN_PATH', '').split(':') if d]
    for d in dirs:
        try:
            if any(n.startswith(name) for n in os.listdir(os.path.join(d, 'platforms'))):
                return None
        except OSError:
            pass
    return 'the Qt 5 %s platform plugin is not installed' % ('Wayland' if name.startswith('libqwayland') else 'xcb')


def cannot_connect():
    """Opens and closes one connection the way Qt's plugin will (libwayland-client or libxcb, which that plugin
    links anyway): Qt aborts without an exception when it can't connect, and this program must still print its
    line. None when it connects or when the check itself can't be done."""
    import ctypes
    import ctypes.util
    want = requested_backend() or ''
    v = os.environ.get('QT_QPA_PLATFORM', '')
    if ';' in v:
        return None
    try:
        if want.startswith('wayland'):
            lib = ctypes.CDLL(ctypes.util.find_library('wayland-client') or 'libwayland-client.so.0')
            lib.wl_display_connect.restype = ctypes.c_void_p
            lib.wl_display_connect.argtypes = [ctypes.c_char_p]
            lib.wl_display_disconnect.argtypes = [ctypes.c_void_p]
            d = lib.wl_display_connect(None)
            if not d:
                return 'could not connect to the Wayland display'
            lib.wl_display_disconnect(d)
        elif want == 'xcb':
            lib = ctypes.CDLL(ctypes.util.find_library('xcb') or 'libxcb.so.1')
            lib.xcb_connect.restype = ctypes.c_void_p
            lib.xcb_connect.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
            lib.xcb_connection_has_error.argtypes = [ctypes.c_void_p]
            lib.xcb_disconnect.argtypes = [ctypes.c_void_p]
            c = lib.xcb_connect(None, None)
            err = lib.xcb_connection_has_error(c) if c else 1
            if c:
                lib.xcb_disconnect(c)
            if err:
                return 'could not connect to the X display (error %d)' % err
    except (OSError, AttributeError):
        return None
    return None


def main():
    # the session's own settings stay as they are: no scale, style or platform is forced here
    why = platform_missing()
    if why:
        result['backend'] = norm_backend(requested_backend())
        for cid in CHECK_IDS:
            skip(cid, why)
        emit_and_exit(3)
        return
    why = cannot_connect()
    if why:                             # the display is there but refused us: that one counts as a failure
        result['backend'] = norm_backend(requested_backend())
        check('map', False, detail=why)
        check('backend', False, detail=why)
        for cid in CHECK_IDS:
            skip(cid, 'no connection')
        emit_and_exit(3)
        return
    try:
        app = QtWidgets.QApplication(['lordos-toolkit-audit'])
    except Exception:
        for cid in CHECK_IDS:
            skip(cid, 'Qt could not start')
        emit_and_exit(3)
        return
    app.setApplicationName('lordos-toolkit-audit')
    app.setQuitOnLastWindowClosed(False)
    win = Win()
    audit = Audit(app, win)
    QtCore.QTimer.singleShot(0, audit.start)
    app.exec_()
    emit_and_exit(0)


if __name__ == '__main__':
    main()
