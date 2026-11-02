#!/usr/bin/python3
"""Instalar o Axia DE: the live session's installation helper.

Before Fedora's installer (liveinst, Anaconda) starts, it checks the boot media against the checksum
implanted in the ISO, so a damaged USB stick is found before a copy fails halfway, and shows the
memory and disks. After the installer closes it reads Anaconda's logs: on a failed copy it says the
likely cause and saves the logs to a file the user can send. Live media only: the installed system
loses it in the image's post-install script.
"""
import io
import json
import locale
import os
import re
import subprocess
import tarfile
import threading
import time

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import Adw, GLib, Gtk  # noqa: E402

ICON = '/usr/libexec/axia/assets/icons/axia-icon-128.png'
ROOT = '/usr/libexec/axia/axia-install-root'   # pkexec, allowed for the live user
MIN_DISK = 20 * 1000 ** 3              # Fedora Workstation with Axia DE: about 9 GB, with room to grow
MIN_RAM = 2 * 1024 ** 3

TEXT = {
    'pt': {
        'title': 'Instalar o Axia DE',
        'intro': 'Instale o Axia DE neste computador. Antes, conferimos se o pendrive está íntegro: '
                 'uma cópia corrompida só aparece no meio da instalação.',
        'checks': 'Verificações',
        'media': 'Mídia de instalação', 'media_run': 'Verificando… {pct} %',
        'media_ok': 'Íntegra', 'media_bad': 'Com defeito: grave o pendrive de novo, de preferência em outro',
        'media_na': 'Não dá para verificar esta mídia (gravada sem a soma de verificação)',
        'media_err': 'Não foi possível verificar: {err}',
        'ram': 'Memória', 'ram_low': '{size}: o mínimo recomendado é 2 GB',
        'disk': 'Disco para instalar', 'disk_none': 'Nenhum disco encontrado',
        'disk_small': 'O maior disco tem {size}: são necessários pelo menos 20 GB',
        'again': 'Verificar de novo', 'install': 'Instalar', 'running': 'O instalador está aberto…',
        'bad_title': 'O pendrive tem defeito', 'bad_body': 'A instalação deve falhar ao copiar os arquivos. '
                     'Grave a ISO de novo, de preferência em outro pendrive, e verifique o arquivo baixado.',
        'cancel': 'Cancelar', 'anyway': 'Instalar mesmo assim',
        'failed': 'A instalação falhou', 'done': 'O instalador foi fechado',
        'done_body': 'Se a instalação terminou, reinicie o computador e retire o pendrive.',
        'cause_media': 'Erro de leitura do pendrive: grave a ISO de novo, em outro pendrive se possível, e verifique a mídia antes.',
        'cause_space': 'Faltou espaço no disco de destino: libere espaço ou escolha usar o disco inteiro.',
        'cause_other': 'Veja as linhas abaixo e o relatório completo.',
        'save': 'Salvar relatório', 'saved': 'Relatório salvo em {path}. Copie-o para outro pendrive para enviar.',
    },
    'en': {
        'title': 'Install Axia DE',
        'intro': 'Install Axia DE on this computer. First we check that the USB stick is intact: '
                 'a damaged copy only shows up halfway through the installation.',
        'checks': 'Checks',
        'media': 'Installation media', 'media_run': 'Checking… {pct} %',
        'media_ok': 'Intact', 'media_bad': 'Damaged: write the USB stick again, preferably another one',
        'media_na': 'This media can\'t be checked (written without the checksum)',
        'media_err': 'Could not check: {err}',
        'ram': 'Memory', 'ram_low': '{size}: at least 2 GB is recommended',
        'disk': 'Disk to install on', 'disk_none': 'No disk found',
        'disk_small': 'The largest disk has {size}: at least 20 GB is needed',
        'again': 'Check again', 'install': 'Install', 'running': 'The installer is open…',
        'bad_title': 'The USB stick is damaged', 'bad_body': 'The installation is likely to fail while copying '
                     'files. Write the ISO again, preferably to another USB stick, and check the download.',
        'cancel': 'Cancel', 'anyway': 'Install anyway',
        'failed': 'The installation failed', 'done': 'The installer was closed',
        'done_body': 'If the installation finished, restart the computer and remove the USB stick.',
        'cause_media': 'The USB stick could not be read: write the ISO again, to another stick if possible, and check the media first.',
        'cause_space': 'The target disk ran out of space: free some space or use the whole disk.',
        'cause_other': 'See the lines below and the full report.',
        'save': 'Save report', 'saved': 'Report saved to {path}. Copy it to another USB stick to send it.',
    },
}
TEXT['es'] = dict(TEXT['en'], **{
    'title': 'Instalar Axia DE', 'install': 'Instalar', 'cancel': 'Cancelar', 'checks': 'Verificaciones',
    'media': 'Medio de instalación', 'media_ok': 'Íntegro', 'ram': 'Memoria', 'disk': 'Disco para instalar',
    'again': 'Verificar de nuevo', 'save': 'Guardar informe', 'failed': 'La instalación falló',
})


def tr(key, **kw):
    lang = (locale.getlocale()[0] or os.environ.get('LANG') or 'en')[:2]
    return TEXT.get(lang, TEXT['en'])[key].format(**kw)


def human(n):
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if n < 1000 or unit == 'TB':
            return f'{n:.0f} {unit}' if unit in ('B', 'KB', 'MB') else f'{n:.1f} {unit}'
        n /= 1000


def live_device():
    """The block device the live system booted from (dracut's dmsquash-live link)."""
    try:
        return os.path.realpath('/run/initramfs/livedev')
    except OSError:
        return None


def disks():
    """Installable disks: whole, writable, not the boot media."""
    out = subprocess.run(['lsblk', '-J', '-b', '-d', '-o', 'NAME,SIZE,TYPE,RO,MODEL'],
                         capture_output=True, text=True, check=False).stdout
    boot = os.path.basename(live_device() or '').rstrip('0123456789')
    found = []
    for d in json.loads(out or '{"blockdevices": []}').get('blockdevices', []):
        if d.get('type') == 'disk' and not d.get('ro') and d['name'] != boot and not d['name'].startswith('zram'):
            found.append((int(d['size']), (d.get('model') or d['name']).strip()))
    return sorted(found, reverse=True)


def read_logs():
    """Anaconda's logs, root's files: {name: text}, through the root helper's tar stream."""
    r = subprocess.run(['pkexec', ROOT, 'logs'], capture_output=True, check=False)
    text = {}
    try:
        with tarfile.open(fileobj=io.BytesIO(r.stdout), mode='r:') as tar:
            for m in tar.getmembers():
                f = tar.extractfile(m) if m.isfile() else None
                if f:
                    text[m.name] = f.read().decode(errors='replace')
    except tarfile.TarError:
        pass
    return text


def diagnose(logs):
    """(failed, cause key, the lines that tell why) from the logs of this run."""
    joined = '\n'.join(logs.values())
    failed = 'PayloadInstallationError' in joined or 'Installation of the system failed' in joined
    lines = [l for l in joined.splitlines()
             if re.search(r'rsync[:(]|Input/output error|No space left|PayloadInstallationError|read errors', l)]
    if re.search(r'Input/output error|read errors|squashfs error|erofs.*(error|corrupt)|I/O error', joined, re.I):
        cause = 'cause_media'
    elif 'No space left on device' in joined:
        cause = 'cause_space'
    else:
        cause = 'cause_other'
    return failed, cause, lines[-40:]


class Helper(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title=tr('title'), default_width=640, default_height=700)
        self.media_state = None
        self.started = time.time()
        view = Adw.ToolbarView()
        view.add_top_bar(Adw.HeaderBar())
        self.set_content(view)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        view.set_content(scroll)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18, margin_top=12, margin_bottom=24,
                      margin_start=24, margin_end=24)
        scroll.set_child(box)
        if os.path.exists(ICON):
            box.append(Gtk.Image(file=ICON, pixel_size=80))
        title = Gtk.Label(label=tr('title'), css_classes=['title-1'])
        box.append(title)
        box.append(Gtk.Label(label=tr('intro'), wrap=True, justify=Gtk.Justification.CENTER))

        group = Adw.PreferencesGroup(title=tr('checks'))
        box.append(group)
        self.media_row = Adw.ActionRow(title=tr('media'))
        self.media_bar = Gtk.ProgressBar(valign=Gtk.Align.CENTER, hexpand=False, width_request=120)
        self.media_row.add_suffix(self.media_bar)
        group.add(self.media_row)
        self.ram_row = Adw.ActionRow(title=tr('ram'))
        group.add(self.ram_row)
        self.disk_row = Adw.ActionRow(title=tr('disk'))
        group.add(self.disk_row)

        self.result = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, visible=False)
        box.append(self.result)
        self.result_title = Gtk.Label(css_classes=['title-3'], xalign=0)
        self.result_body = Gtk.Label(wrap=True, xalign=0, selectable=True)
        self.result_lines = Gtk.TextView(editable=False, monospace=True, wrap_mode=Gtk.WrapMode.CHAR,
                                         height_request=140, visible=False)
        self.save_button = Gtk.Button(label=tr('save'), halign=Gtk.Align.START, visible=False)
        self.save_button.connect('clicked', self.save_report)
        for w in (self.result_title, self.result_body, self.result_lines, self.save_button):
            self.result.append(w)

        buttons = Gtk.Box(spacing=12, halign=Gtk.Align.END)
        box.append(buttons)
        self.again = Gtk.Button(label=tr('again'))
        self.again.connect('clicked', lambda *_: self.check())
        self.install = Gtk.Button(label=tr('install'), css_classes=['suggested-action', 'pill'])
        self.install.connect('clicked', self.on_install)
        buttons.append(self.again)
        buttons.append(self.install)
        self.check()

    # ---- checks ----
    def check(self):
        self.install.set_sensitive(False)
        self.again.set_sensitive(False)
        total = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
        self.ram_row.set_subtitle(tr('ram_low', size=human(total)) if total < MIN_RAM else human(total))
        found = disks()
        if not found:
            self.disk_row.set_subtitle(tr('disk_none'))
        elif found[0][0] < MIN_DISK:
            self.disk_row.set_subtitle(tr('disk_small', size=human(found[0][0])))
        else:
            self.disk_row.set_subtitle(' · '.join(f'{name} ({human(size)})' for size, name in found[:3]))
        self.media_bar.set_visible(True)
        self.media_bar.set_fraction(0)
        self.media_row.set_subtitle(tr('media_run', pct=0))
        threading.Thread(target=self.check_media, daemon=True).start()

    def check_media(self):
        dev = live_device()
        if not dev or not os.path.exists(dev):
            GLib.idle_add(self.media_done, 'err', 'no live device')
            return
        try:
            p = subprocess.Popen(['pkexec', ROOT, 'check'], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        except OSError as e:
            GLib.idle_add(self.media_done, 'err', str(e))
            return
        for line in p.stdout:
            if line.strip().isdigit():
                pct = int(line)
                GLib.idle_add(self.media_progress, pct)
        err = p.stderr.read()
        rc = p.wait()
        state = 'ok' if rc == 0 else 'na' if 'result is: NA' in err else 'bad' if 'result is: FAIL' in err else 'err'
        GLib.idle_add(self.media_done, state, err.strip().splitlines()[-1] if err.strip() else f'exit {rc}')

    def media_progress(self, pct):
        self.media_bar.set_fraction(pct / 100)
        self.media_row.set_subtitle(tr('media_run', pct=pct))

    def media_done(self, state, err):
        self.media_state = state
        self.media_bar.set_visible(False)
        self.media_row.set_subtitle(tr('media_' + state, err=err))
        self.install.set_sensitive(True)
        self.again.set_sensitive(True)

    # ---- installing ----
    def on_install(self, *_):
        if self.media_state == 'bad':
            dialog = Adw.AlertDialog(heading=tr('bad_title'), body=tr('bad_body'))
            dialog.add_response('cancel', tr('cancel'))
            dialog.add_response('anyway', tr('anyway'))
            dialog.set_response_appearance('anyway', Adw.ResponseAppearance.DESTRUCTIVE)
            dialog.connect('response', lambda d, r: r == 'anyway' and self.run_installer())
            dialog.present(self)
            return
        self.run_installer()

    def run_installer(self):
        self.install.set_sensitive(False)
        self.again.set_sensitive(False)
        self.install.set_label(tr('running'))
        self.result.set_visible(False)
        self.started = time.time()
        threading.Thread(target=self.wait_installer, daemon=True).start()

    def wait_installer(self):
        subprocess.run(['liveinst'], check=False)
        GLib.idle_add(self.installer_closed, read_logs())

    def installer_closed(self, logs):
        self.logs = logs
        self.install.set_label(tr('install'))
        self.install.set_sensitive(True)
        self.again.set_sensitive(True)
        failed, cause, lines = diagnose(logs)
        self.result.set_visible(True)
        self.result_title.set_label(tr('failed') if failed else tr('done'))
        self.result_body.set_label(tr(cause) if failed else tr('done_body'))
        self.result_lines.get_buffer().set_text('\n'.join(lines))
        self.result_lines.set_visible(failed and bool(lines))
        self.save_button.set_visible(bool(logs))

    def save_report(self, *_):
        path = os.path.expanduser(time.strftime('~/axia-instalacao-%Y%m%d-%H%M%S.tar.gz'))
        with tarfile.open(path, 'w:gz') as tar:
            for name, text in self.logs.items():
                data = text.encode()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mtime = int(time.time())
                tar.addfile(info, io.BytesIO(data))
        self.result_body.set_label(self.result_body.get_label() + '\n\n' + tr('saved', path=path))


def main():
    locale.setlocale(locale.LC_ALL, '')
    app = Adw.Application(application_id='org.axia.Install')     # the .desktop file's name
    app.connect('activate', lambda a: (a.get_active_window() or Helper(a)).present())
    return app.run(None)


if __name__ == '__main__':
    raise SystemExit(main())
