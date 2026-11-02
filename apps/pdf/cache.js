// cache.js — pages already rendered by axia-doc, with a memory cap. While the version
// at the new zoom arrives, whatever exists for that page is shown (smaller or larger, scaled):
// so drawing is progressive and there is never a gap.
import { releaseImage } from '@lordos/sdk';

const quant = (w) => Math.max(32, Math.round(w / 32) * 32);

export class PageCache {
  constructor(doc, onChange, budget = 48 * 1024 * 1024) {
    this.doc = doc;
    this.onChange = onChange;
    this.budget = budget;
    this.map = new Map();          // key → { image, w, h, page, width, rot, inv, bytes, used }
    this.pending = new Map();      // key → promise
    this.bytes = 0;
    this.tick = 0;
    this.gen = new Map();          // page → generation (bumped when a form or an annotation changes)
    this.firstAt = 0;
  }
  key(page, width, rot, inv) { return `${page}|${quant(width)}|${rot}|${inv ? 1 : 0}|${this.gen.get(page) || 0}`; }
  get(page, width, rot, inv) {
    const e = this.map.get(this.key(page, width, rot, inv));
    if (e) e.used = ++this.tick;
    return e || null;
  }
  // the best one available for the page (to show while the good one arrives)
  best(page, rot, inv, width) {
    let best = null;
    const g = this.gen.get(page) || 0;
    const score = (e) => (e.gen === g ? 0 : 1e6) + Math.abs(e.width - width);
    for (const e of this.map.values()) {
      if (e.page !== page || e.rot !== rot || e.inv !== inv) continue;
      if (!best || score(e) < score(best)) best = e;
    }
    return best;
  }
  want(page, width, rot, inv, prio) {
    const k = this.key(page, width, rot, inv);
    if (this.map.has(k) || this.pending.has(k)) return;
    const g = this.gen.get(page) || 0;
    const t0 = Date.now();
    const p = this.doc.render(page, quant(width), { rot, invert: inv, prio });
    this.pending.set(k, p);
    p.then((r) => {
      this.pending.delete(k);
      if (this.closed || (this.gen.get(page) || 0) !== g) { releaseImage(r.image); return; }
      const e = { image: r.image, w: r.w, h: r.h, page, width: quant(width), rot, inv, gen: g, bytes: r.w * r.h * 4, used: ++this.tick, ms: Date.now() - t0 };
      this.map.set(k, e);
      this.bytes += e.bytes;
      if (!this.firstAt) this.firstAt = Date.now();
      this.purgeStale(page);
      this.evict(k);
      this.onChange();
    }).catch(() => { this.pending.delete(k); });
  }
  // requests no longer needed (the page left the view before starting)
  drop(keep) {
    for (const [k, p] of this.pending) if (!keep.has(Number(k.split('|')[0]))) { this.doc.cancel(p); }
  }
  evict(except) {
    if (this.bytes <= this.budget) return;
    const list = [...this.map.entries()].filter(([k]) => k !== except).sort((a, b) => a[1].used - b[1].used);
    for (const [k, e] of list) {
      if (this.bytes <= this.budget * 0.8) break;
      releaseImage(e.image);
      this.bytes -= e.bytes;
      this.map.delete(k);
    }
  }
  // the page changed (form, annotation): the old versions keep being shown until
  // the new one arrives, and are discarded afterwards
  invalidate(page) {
    const g = (this.gen.get(page) || 0) + 1;
    this.gen.set(page, g);
    this.onChange();
  }
  purgeStale(page) {
    const g = this.gen.get(page) || 0;
    for (const [k, e] of this.map) if (e.page === page && e.gen !== g) { releaseImage(e.image); this.bytes -= e.bytes; this.map.delete(k); }
  }
  clear() {
    for (const e of this.map.values()) releaseImage(e.image);
    this.map.clear();
    this.bytes = 0;
  }
  close() { this.closed = true; this.clear(); }
}
