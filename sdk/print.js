// print.js — printing and printers (CUPS over IPP from our own C client, lord.print) and
// network eSCL scanners. See docs/PRINTING.md.
//
// For apps:
//   print.open({ title, path })            print a PDF (opens the system dialog)
//   print.open({ title, doc })             a PDF already opened with pdf.open (with its annotations)
//   print.open({ title, text, mono })      text (Editor, Notes): a PDF is built in axia-doc
//   print.open({ title, image })           an image (Photos, the scanner)
//     → promise: { job, printer } | { saved: path } | null (cancelled)
//
// What follows is used by Settings → Printers and the Scanner:
//   printers() → { printers: [{ name, info, location, model, state, message, accepting, color, media, sides, colorModes }], default }
//   jobs(), cancel(id), add(name, uri, info, location), remove(name), setDefault(name)
//   discover() → { printers: [{ name, uri, addr, port, model, uuid, color, duplex }], scanners: [{ name, addr, port, rs, model }] }
//   scanCaps(scanner), scan(scanner, settings) → { file, format, bytes }
//   submit(printer, blob | pdfPath, options) → { job }
import { createStore } from './store.js';
import { tOptional } from './i18n.js';
import { errorText } from './errors.js';

// lord.print reports short codes ('no-cups', 'unreachable'…): the message is their translation (print.err.*).
const printError = (code, detail) => Object.assign(new Error(code ? tOptional(`print.err.${code}`, { detail: detail || '' }) ?? errorText(code) : errorText('sys.unknown')), { code, detail });

const has = () => typeof lord !== 'undefined' && typeof lord.print !== 'undefined';
const waiting = new Map();
let timer = null;

function pump() {
  let evs = null;
  try { evs = lord.print.poll(); } catch (e) { evs = null; }
  for (const e of evs || []) {
    const w = waiting.get(e.id);
    if (!w) continue;
    waiting.delete(e.id);
    if (e.ok) w.resolve(e.data);
    else w.reject(printError(e.data?.error, e.data?.detail));
  }
  if (!waiting.size && timer) { clearInterval(timer); timer = null; }
}

function wait(id) {
  if (!(id > 0)) return Promise.reject(printError('busy'));
  return new Promise((resolve, reject) => {
    waiting.set(id, { resolve, reject });
    if (!timer) timer = setInterval(pump, 50);
  });
}

const call = (fn) => { try { return wait(fn()); } catch (e) { return Promise.reject(e); } };

// System dialog (drawn by <PrintDialog />, which startShell mounts)
export const printRequest = createStore(null);    // { source, resolve }

export const print = {
  available: () => has(),
  hasCups: () => has() && lord.print.hasCups(),
  printers: () => call(() => lord.print.printers()),
  jobs: () => call(() => lord.print.jobs()),
  cancel: (id) => call(() => lord.print.cancel(id)),
  add: (name, uri, info = '', location = '') => call(() => lord.print.add(name, uri, info, location)),
  remove: (name) => call(() => lord.print.remove(name)),
  setDefault: (name) => call(() => lord.print.setDefault(name)),
  discover: () => call(() => lord.print.discover()),
  scanCaps: (s) => call(() => lord.print.scanCaps(s.addr, s.port, s.rs || 'eSCL')),
  scan: (s, settings) => call(() => lord.print.scan(s.addr, s.port, s.rs || 'eSCL', JSON.stringify(settings))),
  submit: (printer, source, options = {}) => call(() => lord.print.submit(printer, source, JSON.stringify(options))),
  // Opens the dialog. If one is already open, the new one waits its turn (returns null).
  open(source) {
    if (printRequest.get()) return Promise.resolve(null);
    return new Promise((resolve) => printRequest.set({ source, resolve }));
  },
};

// CUPS name from a free-form name: "Office (HP)" → "Office_HP"
export const queueName = (s) => String(s).normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^A-Za-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 60) || 'Impresora';
