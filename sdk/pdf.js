// pdf.js — PDF through the confined helper (Poppler inside axia-doc; see docs/DOCUMENTS.md).
//
//   const doc = await pdf.open(path | { blob }, { quick })
//   doc.info           { pages, sizes: [[w, h]…] (points), title, author, outline, forms }
//   doc.render(page, width, { rot (0..3), invert, prio }) → { image, w, h }   (release with releaseImage)
//   doc.select(page, x1, y1, x2, y2, style)           → { text, rects }   (points, top-left)
//   doc.find(text, { case, whole }, (page, rects) => …) → { total }
//   doc.outline() → { items: [{ title, page, kids }] }
//   doc.pageInfo(page) → { links, fields, notes }
//   doc.setField(id, value), doc.annot({ page, type, … }), doc.undo(page)
//   doc.save(path) (new copy), doc.blob() → { blob }, doc.sizes(), doc.close()
//   pdf.thumb(path, { width, height }) → thumbnail of page 1 without opening a session
import { request, waitFor, onSessionCrash, pdfThumb, cancelRequest } from './doc.js';

class PdfDoc {
  constructor(sid, info) { this.sid = sid; this.info = info; this.closed = false; }
  render(page, width, { rot = 0, invert = false, prio = 1 } = {}) {
    // rot in quarter turns (0..3); the helper receives degrees
    return request(this.sid, 'render', { page, width: Math.round(width), rot: ((rot % 4) + 4) % 4 * 90, invert }, { prio });
  }
  select(page, x1, y1, x2, y2, style = 0) { return request(this.sid, 'select', { page, x1, y1, x2, y2, style }, { prio: 5 }); }
  find(text, opts = {}, onPage) {
    return request(this.sid, 'find', { text, case: !!opts.case, whole: !!opts.whole }, { prio: 3, onPart: (d) => onPage?.(d.page, d.rects) });
  }
  outline() { return request(this.sid, 'outline', null, { prio: 4 }); }
  pageInfo(page) { return request(this.sid, 'pageinfo', { page }, { prio: 2 }); }
  setField(id, value) { return request(this.sid, 'setfield', { id, value }, { prio: 6 }); }
  annot(a) { return request(this.sid, 'annot', a, { prio: 6 }); }
  undo(page) { return request(this.sid, 'undo', { page }, { prio: 6 }); }
  sizes() { return request(this.sid, 'sizes', null, { prio: 0 }); }
  blob() { return request(this.sid, 'blob', null, { prio: 6 }); }
  save(path) {
    let req;
    try { req = lord.doc.savePdf(this.sid, path); } catch (e) { return Promise.reject(e); }
    return waitFor(this.sid, req);
  }
  cancel(p) { if (p?.req) cancelRequest(this.sid, p.req); }
  onCrash(fn) { return onSessionCrash(this.sid, fn); }
  close() { if (!this.closed) { this.closed = true; lord.doc.close(this.sid); } }
}

async function open(src, { quick = false, password = '' } = {}) {
  const sid = typeof src === 'object' && src && src.blob ? lord.doc.openBlob(src.blob) : lord.doc.openPdf(src);
  try {
    const info = await request(sid, 'open', { slot: 0, quick, password }, { prio: 9 });
    return new PdfDoc(sid, info);
  } catch (e) {
    lord.doc.close(sid);
    throw e;
  }
}

export const pdf = { open, thumb: pdfThumb };
