#!/usr/bin/python3
# tools/toolkit-audit/qt6.py — the Qt 6 harness of `lordos-testbench --toolkits` (installed to
# /usr/lib/lordos/toolkit-audit/qt6.py by tools/iso/mkdeb.sh).
#
# It opens one 600x400 window with PyQt6, runs the checks it can see from inside the toolkit, prints ONE JSON
# line on stdout and exits by itself within 5 s:
#   {"toolkit":"qt6","version":"6.x.y","backend":"wayland"|"xcb","checks":{id:{"ok":bool|null,"ms"?:n,"detail"?:str}}}
# with the check ids every harness shares: map, size, scale, theme, backend, decoration, first_frame, menu, combo,
# tooltip, clipboard, dnd, text, fullscreen. A check it can't do is {"ok":null,"detail":"skipped: why"}.
#
# The backend is the one the orchestrator asks for with QT_QPA_PLATFORM (wayland or xcb); the orchestrator also
# sets WAYLAND_DEBUG=1 and reads the globals bound from stderr. No test hooks: only the toolkit's public API and,
# for input, the toolkit's own synthetic events.
#
# LordOS doesn't depend on Qt or PyQt6: without them this prints the same line with "installed":false, every
# check skipped, and exits with 77. It never installs anything.
#
# Privacy: the line carries only the toolkit, its version, the backend, numbers and check ids; no names, paths or
# titles. The window shows no words (icons, digits and symbols only), so it needs no translations.
import json
import os
import random
import sys
import threading
import time

T0 = time.monotonic()
CHECK_IDS = ('map', 'size', 'scale', 'theme', 'backend', 'decoration', 'first_frame', 'menu', 'combo', 'tooltip',
             'clipboard', 'dnd', 'text', 'fullscreen')
WANT_W, WANT_H = 600, 400
SELF_EXIT_S = 4.6      # everything done or not, the line is printed by then (the orchestrator kills at 8 s)

_out_lock = threading.RLock()   # put() and the final print: the watchdog thread prints while the checks run
_printed = False
RESULT = {'toolkit': 'qt6', 'version': None, 'backend': None, 'checks': {}}


def ms_since(t):
    return int(round((time.monotonic() - t) * 1000))


def put(cid, ok, ms=None, detail=None):
    c = {'ok': ok}
    if ms is not None:
        c['ms'] = int(ms)
    if detail:
        c['detail'] = str(detail)[:160]
    with _out_lock:
        if not _printed:
            RESULT['checks'][cid] = c


def skip(cid, why):
    put(cid, None, detail='skipped: ' + why)


def emit_and_exit(code=0, missing='timeout: not finished before the harness had to exit'):
    """Prints the line once (from the main thread or the watchdog) and leaves without Qt's teardown."""
    global _printed
    with _out_lock:
        if not _printed:
            for cid in CHECK_IDS:
                if cid not in RESULT['checks']:
                    RESULT['checks'][cid] = {'ok': False, 'detail': missing}
            RESULT['checks'] = {cid: RESULT['checks'][cid] for cid in CHECK_IDS}
            _printed = True
            try:
                sys.stdout.write(json.dumps(RESULT, separators=(',', ':')) + '\n')
                sys.stdout.flush()
            except Exception:
                pass
    os._exit(code)


try:
    from PyQt6.QtCore import (QEvent, QLocale, QMimeData, QPoint, QT_VERSION_STR, Qt, QtMsgType, QTimer,
                              qInstallMessageHandler, qVersion)
    from PyQt6.QtGui import QDrag, QGuiApplication, QKeyEvent, QPainter
    from PyQt6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFrame, QHBoxLayout, QLabel, QLineEdit,
                                 QMainWindow, QMenu, QPushButton, QSlider, QStyle, QToolTip, QVBoxLayout, QWidget)
except Exception:
    RESULT['installed'] = False
    for cid in CHECK_IDS:
        skip(cid, 'PyQt6 is not installed')
    emit_and_exit(77)

try:
    from PyQt6.QtDBus import QDBus, QDBusConnection, QDBusMessage
except Exception:
    QDBusConnection = None

# The watchdog: prints what is known and exits even if a toolkit call never returns.
_wd = threading.Timer(SELF_EXIT_S, emit_and_exit)
_wd.daemon = True
_wd.start()


# Qt's own warnings still reach stderr (the orchestrator scans it); the harness also remembers whether Qt Wayland
# refused a grabbing popup because the window never got input (a press or a key: the serial xdg_popup.grab needs).
QT_SAID = {'no_grab_serial': 0}


def _qt_messages(kind, ctx, msg):
    if 'Failed to create grabbing popup' in msg:
        QT_SAID['no_grab_serial'] += 1
    try:
        sys.stderr.write(msg + '\n')
        sys.stderr.flush()
    except Exception:
        pass
    if kind == QtMsgType.QtFatalMsg:     # e.g. no platform plugin could connect: still one line, then out
        emit_and_exit(0, 'fatal: Qt stopped before this check (its message is on stderr)')


qInstallMessageHandler(_qt_messages)


def exposed(w):
    h = w.windowHandle() if w is not None else None
    return bool(h is not None and w.isVisible() and h.isExposed())


class Canvas(QWidget):
    """The central widget: records when its first frame is painted."""

    def __init__(self, on_first_paint):
        super().__init__()
        self._first = on_first_paint

    def paintEvent(self, ev):
        super().paintEvent(ev)
        if self._first is not None:
            f, self._first = self._first, None
            f()


class DropTarget(QFrame):
    def __init__(self):
        super().__init__()
        self.setAcceptDrops(True)
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setMinimumSize(48, 32)
        self.dropped = None

    def dragEnterEvent(self, ev):
        if ev.mimeData().hasFormat('application/x-lordos-audit'):
            ev.acceptProposedAction()

    def dragMoveEvent(self, ev):
        ev.acceptProposedAction()

    def dropEvent(self, ev):
        self.dropped = bytes(ev.mimeData().data('application/x-lordos-audit')).decode('ascii', 'replace')
        ev.acceptProposedAction()


class Indicator(QLabel):
    """The light or dark indicator: a swatch of the window colour with a sun or a moon."""

    def __init__(self):
        super().__init__()
        self.setFixedSize(28, 28)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.dark = None

    def set_dark(self, dark):
        self.dark = dark
        self.setText('☾' if dark else '☀')
        self.update()

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setPen(self.palette().color(self.foregroundRole()))
        p.setBrush(self.palette().color(self.backgroundRole()))
        p.drawRoundedRect(self.rect().adjusted(1, 1, -1, -1), 5, 5)
        p.end()
        super().paintEvent(ev)


class Audit:
    def __init__(self, app):
        self.app = app
        self.t_app = time.monotonic()
        # when the keyboard focus last moved between this app's windows (wl_keyboard.enter / FocusIn)
        self.focus_changed = self.t_app
        app.focusWindowChanged.connect(self.on_focus)
        self.t_show = None
        self.t_paint = None
        self.gen = None
        self.busy = False
        self.cond = None
        self.deadline = 0
        self.timer = QTimer()
        self.timer.setInterval(10)
        self.timer.timeout.connect(self.tick)
        self.build()

    # ---------------------------------------------------------------- the window
    def build(self):
        st = self.app.style()
        icon = st.standardIcon
        w = self.win = QMainWindow()
        w.setWindowTitle('lordos-testbench')
        self.menu = QMenu(w)
        self.menu.setIcon(icon(QStyle.StandardPixmap.SP_FileDialogDetailedView))
        self.menu.addAction(icon(QStyle.StandardPixmap.SP_DialogOpenButton), '')
        self.menu.addAction(icon(QStyle.StandardPixmap.SP_DialogSaveButton), '')
        self.menu.addSeparator()
        self.menu.addAction(icon(QStyle.StandardPixmap.SP_DialogCloseButton), '')
        self.menu_action = w.menuBar().addMenu(self.menu)

        c = self.canvas = Canvas(self.first_paint)
        v = QVBoxLayout(c)
        top = QHBoxLayout()           # strictly start- and end-aligned; Qt mirrors it in a right-to-left locale
        self.b_start = QPushButton(icon(QStyle.StandardPixmap.SP_ArrowBack), '')
        self.b_end = QPushButton(icon(QStyle.StandardPixmap.SP_ArrowForward), '')
        top.addWidget(self.b_start, 0, Qt.AlignmentFlag.AlignLeading)
        top.addStretch(1)
        self.indicator = Indicator()
        top.addWidget(self.indicator)
        top.addStretch(1)
        top.addWidget(self.b_end, 0, Qt.AlignmentFlag.AlignTrailing)
        v.addLayout(top)

        self.entry = QLineEdit()
        v.addWidget(self.entry)
        row = QHBoxLayout()
        self.check = QCheckBox()
        row.addWidget(self.check)
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(0, 100)
        self.slider.setValue(40)
        row.addWidget(self.slider, 1)
        self.combo = QComboBox()
        self.combo.addItems(['1', '2', '3', '4'])
        row.addWidget(self.combo)
        v.addLayout(row)

        dnd = QHBoxLayout()
        self.drag_src = QLabel()
        self.drag_src.setPixmap(icon(QStyle.StandardPixmap.SP_FileIcon).pixmap(24, 24))
        self.drop = DropTarget()
        dnd.addWidget(self.drag_src)
        dnd.addStretch(1)
        dnd.addWidget(self.drop)
        v.addLayout(dnd)
        v.addStretch(1)
        w.setCentralWidget(c)
        w.resize(WANT_W, WANT_H)

    def first_paint(self):
        self.t_paint = time.monotonic()

    # ---------------------------------------------------------------- the step runner
    def start(self):
        self.gen = self.steps()
        self.timer.start()
        self.advance(None)

    def advance(self, val):
        self.busy = True
        try:
            req = self.gen.send(val)
        except StopIteration:
            self.timer.stop()
            emit_and_exit(0)
            return
        except Exception as e:      # a harness bug still gives a line: the checks not done carry the error's type
            emit_and_exit(0, 'harness error: ' + type(e).__name__)
            return
        finally:
            self.busy = False
        self.cond, timeout_ms = req
        self.deadline = time.monotonic() + timeout_ms / 1000.0

    def tick(self):
        if self.busy or self.cond is None:
            return
        try:
            done = bool(self.cond())
        except Exception:
            done = False
        if done or time.monotonic() >= self.deadline:
            self.cond = None
            self.advance(done)

    # ---------------------------------------------------------------- the checks
    def steps(self):
        w = self.win
        want = (os.environ.get('QT_QPA_PLATFORM') or '').split(';')[0].split(':')[0].strip()
        platform = QGuiApplication.platformName()
        RESULT['backend'] = platform

        # backend: the platform plugin Qt really loaded, against the one asked for
        if want:
            put('backend', platform == want, detail=platform + ' (asked ' + want + ')')
        else:
            put('backend', bool(platform), detail=platform + ' (none asked)')

        # map + first_frame
        self.t_show = time.monotonic()
        w.show()
        ok = yield (lambda: exposed(w), 2000)
        put('map', ok, ms=ms_since(self.t_show), detail=None if ok else 'the window was not exposed in 2 s')
        if not ok:
            for cid in ('size', 'scale', 'decoration', 'menu', 'combo', 'tooltip', 'dnd', 'text', 'fullscreen'):
                skip(cid, 'the window never mapped')
        ok = yield (lambda: self.t_paint is not None, 1000 if ok else 10)
        if ok:
            put('first_frame', True, ms=int(round((self.t_paint - self.t_app) * 1000)),
                detail='from the process start; show to paint %d ms' % int(round((self.t_paint - self.t_show) * 1000)))
        else:
            put('first_frame', False, detail='no frame painted')
        if 'size' in RESULT['checks']:      # never mapped
            self.theme_check()
            self.clipboard_start()
            yield (lambda: self.clip_done(), 600)
            self.clipboard_end()
            return

        # size: the client area the compositor gave us (decorations not counted), after configure settles
        yield (lambda: False, 120)
        s = w.size()
        put('size', s.width() == WANT_W and s.height() == WANT_H, detail='%dx%d' % (s.width(), s.height()))

        # scale: the window's device pixel ratio (Wayland's preferred/fractional scale, X11's Xft.dpi)
        h = w.windowHandle()
        dpr = h.devicePixelRatio()
        scr = h.screen()
        sdpr = scr.devicePixelRatio() if scr else 0
        put('scale', dpr > 0, detail='%g (screen %g, logical dpi %g)' % (dpr, sdpr, scr.logicalDotsPerInch() if scr else 0))

        # decoration: Qt draws its own frame (client side) when it has frame margins
        m = h.frameMargins()
        marg = (m.left(), m.top(), m.right(), m.bottom())
        if any(marg):
            put('decoration', True, detail='client-side frame %d,%d,%d,%d' % marg if platform.startswith('wayland')
                else 'window manager frame extents %d,%d,%d,%d' % marg)
        elif platform.startswith('wayland') and os.environ.get('QT_WAYLAND_DISABLE_WINDOWDECORATION') == '1':
            put('decoration', False, detail='none: QT_WAYLAND_DISABLE_WINDOWDECORATION=1')
        else:
            put('decoration', True, detail='server-side (no frame margins; zxdg_toplevel_decoration_v1 in the protocol log)'
                if platform.startswith('wayland') else 'server-side (no _NET_FRAME_EXTENTS)')

        self.theme_check()

        # menu: the menu bar's menu opens as a popup and maps
        t = time.monotonic()
        said = QT_SAID['no_grab_serial']
        pos = w.menuBar().mapToGlobal(w.menuBar().actionGeometry(self.menu_action).bottomLeft())
        self.menu.popup(pos)
        yield (lambda: exposed(self.menu) or QT_SAID['no_grab_serial'] > said, 1000)
        self.popup_result('menu', exposed(self.menu), said, t)
        self.menu.hide()
        yield (lambda: not self.menu.isVisible(), 300)
        # a menu opened as a toplevel (no input serial yet): the compositor gives the keyboard back to the window a
        # frame or so after it is gone, and Qt closes a combo list that is open when that happens (a focus change
        # closes popups). A person never opens the combo before that either, so wait, as qt5.py's settle() does,
        # until the window has the keyboard again and it has been quiet for 100 ms (at most 700 ms).
        # focusWindow follows the compositor's keyboard focus; isActiveWindow doesn't (Qt sets it when a popup closes).
        yield self.settled()

        # combo: the combo box's list popup opens and maps
        t = time.monotonic()
        said = QT_SAID['no_grab_serial']
        self.combo.showPopup()
        pop = self.combo.view().window()
        yield (lambda: (pop is not w and exposed(pop)) or QT_SAID['no_grab_serial'] > said, 1000)
        self.popup_result('combo', pop is not w and exposed(pop), said, t)
        self.combo.hidePopup()
        yield (lambda: not pop.isVisible(), 300)
        yield self.settled()                # the combo list too, before the tooltip (a focus change hides it)

        # tooltip: shown by the toolkit, it maps as its own popup
        t = time.monotonic()
        QToolTip.showText(self.b_end.mapToGlobal(self.b_end.rect().center()), qVersion(), self.b_end)

        def tip():
            for tw in QApplication.topLevelWidgets():
                if tw.inherits('QTipLabel') and exposed(tw):
                    return True
            return False
        ok = yield (tip, 1000)
        put('tooltip', ok, ms=ms_since(t), detail=None if ok else 'the tooltip did not map in 1 s')
        QToolTip.hideText()

        # clipboard: copy a random token and read it back within this process
        self.clipboard_start()
        yield (lambda: self.clip_done(), 600)
        self.clipboard_end()

        # text: focus the line edit and type with the toolkit's own key events
        t = time.monotonic()
        w.activateWindow()
        self.entry.setFocus()
        typed = 'Lord 2026'
        for ch in typed:
            key = Qt.Key.Key_Space if ch == ' ' else (Qt.Key(ord(ch.upper())) if ch.isalnum() else Qt.Key.Key_unknown)
            mods = Qt.KeyboardModifier.ShiftModifier if ch.isupper() else Qt.KeyboardModifier.NoModifier
            QApplication.sendEvent(self.entry, QKeyEvent(QEvent.Type.KeyPress, key, mods, ch))
            QApplication.sendEvent(self.entry, QKeyEvent(QEvent.Type.KeyRelease, key, mods, ch))
        ok = yield (lambda: self.entry.text() == typed, 300)
        put('text', ok, ms=ms_since(t), detail='typed %d of %d characters' % (len(self.entry.text()), len(typed))
            + ('' if self.entry.hasFocus() else '; the field had no keyboard focus'))

        # dnd: a drag from the icon to the frame in this window, moved with the toolkit's own mouse events
        yield from self.dnd_steps(platform)

        # fullscreen: on, then back to the normal size
        t = time.monotonic()
        before = w.size()
        w.showFullScreen()
        scr = w.windowHandle().screen()
        full = scr.geometry().size() if scr else None
        on = yield (lambda: bool(w.windowState() & Qt.WindowState.WindowFullScreen)
                    and (full is None or w.size() == full or w.width() > WANT_W), 1200)
        on_size = '%dx%d' % (w.width(), w.height())
        w.showNormal()
        off = yield (lambda: not (w.windowState() & Qt.WindowState.WindowFullScreen)
                     and w.size() == before, 1200)
        put('fullscreen', on and off, ms=ms_since(t), detail='on %s (screen %s), off %dx%d (before %dx%d)' % (
            on_size, '%dx%d' % (full.width(), full.height()) if full else '?', w.width(), w.height(),
            before.width(), before.height()))

    def popup_result(self, cid, ok, said, t):
        if ok:
            put(cid, True, ms=ms_since(t))
        elif QT_SAID['no_grab_serial'] > said:
            skip(cid, 'Qt opens a grabbing Wayland popup only after the window got a press or a key (xdg_popup.grab needs its serial)')
        else:
            put(cid, False, ms=ms_since(t), detail='the popup did not map in 1 s')

    def settled(self):
        """A wait (for steps()): until the main window has the keyboard again and it has been quiet for 100 ms, at
        most 700 ms. focusWindow follows the compositor's keyboard focus; isActiveWindow doesn't."""
        t = time.monotonic()
        return (lambda: time.monotonic() - t >= 0.1 and time.monotonic() - self.focus_changed >= 0.1
                and QGuiApplication.focusWindow() is self.win.windowHandle(), 700)

    def on_focus(self, _win):
        self.focus_changed = time.monotonic()

    def theme_check(self):
        """theme: the colour scheme Qt follows, and the portal's own setting when the session bus has one."""
        qt = 'unknown'
        try:
            cs = self.app.styleHints().colorScheme()
            qt = {Qt.ColorScheme.Dark: 'dark', Qt.ColorScheme.Light: 'light'}.get(cs, 'unknown')
        except Exception:
            pass
        portal = self.portal_scheme()
        win = self.app.palette().color(self.app.palette().ColorRole.Window)
        pal = 'dark' if win.lightness() < 128 else 'light'
        self.indicator.set_dark((qt if qt != 'unknown' else pal) == 'dark')
        # without a platform theme Qt 6 reports no colour scheme: then its palette is what an app shows
        tk = qt if qt != 'unknown' else pal
        if portal in ('dark', 'light'):
            ok = tk == portal
        else:
            ok = True
        put('theme', ok, detail='qt %s, palette %s, portal %s' % (qt, pal, portal))

    def portal_scheme(self):
        if QDBusConnection is None:
            return 'no QtDBus'
        try:
            bus = QDBusConnection.sessionBus()
            if not bus.isConnected():
                return 'no session bus'
            msg = QDBusMessage.createMethodCall('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                                                'org.freedesktop.portal.Settings', 'ReadOne')
            msg.setArguments(['org.freedesktop.appearance', 'color-scheme'])
            rep = bus.call(msg, QDBus.CallMode.Block, 500)
            if rep.type() != QDBusMessage.MessageType.ReplyMessage or not rep.arguments():
                return 'no answer'
            v = rep.arguments()[0]
            for _ in range(3):      # a variant, sometimes inside another
                if hasattr(v, 'variant'):
                    v = v.variant()
            v = int(v)
            return {1: 'dark', 2: 'light'}.get(v, 'no preference')
        except Exception:
            return 'unreadable'

    def clipboard_start(self):
        self.clip_token = 'lordos-audit-%08x' % random.getrandbits(32)
        self.clip_t = time.monotonic()
        try:
            self.app.clipboard().setText(self.clip_token)
        except Exception:
            pass

    def clip_done(self):
        try:
            return self.app.clipboard().text() == self.clip_token
        except Exception:
            return False

    def clipboard_end(self):
        cb = self.app.clipboard()
        ok = self.clip_done()
        try:
            owner = cb.ownsClipboard()
        except Exception:
            owner = False
        put('clipboard', ok, ms=ms_since(self.clip_t), detail='read back %s; owner %s' % (
            'the same text' if ok else 'something else', 'yes' if owner else 'no'))

    def dnd_steps(self, platform):
        t = time.monotonic()
        state = {'result': None, 'running': False}
        src, dst = self.drag_src, self.drop
        token = 'lordos-dnd-%08x' % random.getrandbits(32)

        def mouse(target, etype, gpos, buttons):
            from PyQt6.QtGui import QMouseEvent
            from PyQt6.QtCore import QPointF
            lp = QPointF(target.mapFromGlobal(gpos))
            btn = Qt.MouseButton.LeftButton if etype != QEvent.Type.MouseMove else Qt.MouseButton.NoButton
            QApplication.postEvent(target, QMouseEvent(etype, lp, QPointF(gpos), btn, buttons,
                                                        Qt.KeyboardModifier.NoModifier))

        a = src.mapToGlobal(src.rect().center())
        b = dst.mapToGlobal(dst.rect().center())

        def run():
            md = QMimeData()
            md.setData('application/x-lordos-audit', token.encode('ascii'))
            d = QDrag(src)
            d.setMimeData(md)
            d.setPixmap(src.pixmap())
            state['running'] = True
            # moves and the release reach the drag's event filter while exec() runs its own loop
            for i, f in enumerate((0.25, 0.5, 0.75, 1.0)):
                p = QPoint(int(a.x() + (b.x() - a.x()) * f), int(a.y() + (b.y() - a.y()) * f))
                QTimer.singleShot(60 * (i + 1), lambda p=p: mouse(dst, QEvent.Type.MouseMove, p, Qt.MouseButton.LeftButton))
            QTimer.singleShot(400, lambda: mouse(dst, QEvent.Type.MouseButtonRelease, b, Qt.MouseButton.NoButton))
            try:
                state['result'] = d.exec(Qt.DropAction.CopyAction)
            except Exception:
                state['result'] = Qt.DropAction.IgnoreAction
            state['running'] = False
            state['ms'] = ms_since(t)

        QTimer.singleShot(0, run)
        yield (lambda: dst.dropped is not None or (state['result'] is not None and not state['running']), 1300)
        if state['running']:
            try:
                QDrag.cancel()
            except Exception:
                pass
            yield (lambda: not state['running'], 200)
        if dst.dropped == token:
            put('dnd', True, ms=ms_since(t), detail='dropped in the same window')
        elif platform.startswith('wayland') and state.get('ms', 9999) < 100 and dst.dropped is None:
            skip('dnd', 'Qt starts a Wayland drag only from a real pointer press (wl_data_device.start_drag needs its serial)')
        else:
            put('dnd', False, ms=ms_since(t), detail='no drop arrived' if dst.dropped is None else 'the drop carried other data')


def main():
    os.environ.setdefault('QT_ACCESSIBILITY', '0')
    app = QApplication(['lordos-toolkit-audit-qt6'])
    app.setDesktopFileName('lordos-toolkit-audit-qt6')     # the Wayland app_id and the X11 WM_CLASS, not "python3"
    RESULT['version'] = qVersion() or QT_VERSION_STR
    loc = QLocale()
    if loc.textDirection() == Qt.LayoutDirection.RightToLeft:
        app.setLayoutDirection(Qt.LayoutDirection.RightToLeft)
    audit = Audit(app)
    audit.t_app = T0
    QTimer.singleShot(0, audit.start)
    app.exec()
    emit_and_exit(0)


if __name__ == '__main__':
    main()
