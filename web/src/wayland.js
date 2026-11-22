// lordos.org/wayland-protocols-table/ only (loaded by that page, not part of site.js's size budget).
(function () {
  'use strict';
  var d = document;
  var $ = function (s, r) { return (r || d).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || d).querySelectorAll(s)); };
  // Wayland protocol table: search, category, LordOS's status and who offers it. Rows carry data-cat, data-st,
  // data-dev, data-cf (the conformance depth, or none) and data-l/k/m (LordOS, KWin, Mutter offer it: 1 or 0); the
  // filters are kept in the address (#q=…).
  var wpT = $('#wp-table');
  if (wpT) {
    var wpRows = $$('tbody tr', wpT), wq = $('#wp-q'), wcat = $('#wp-cat'), wst = $('#wp-st'), wcomp = $('#wp-comp'), wcf = $('#wp-cf');
    var wnorm = function (s) { return s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, ''); };
    var wcount = $('#wp-count'), wtpl = wcount.getAttribute('data-tpl');
    var on = function (r, k) { return r.getAttribute('data-' + k) === '1'; };
    var wapply = function () {
      var q = wnorm(wq.value.trim()), c = wcat.value, st = wst.value, cp = wcomp.value, cf = wcf ? wcf.value : 'all', n = 0;
      wpRows.forEach(function (r) {
        var ok = (!q || wnorm(r.getAttribute('data-q')).indexOf(q) >= 0) &&
          (c === 'all' || r.getAttribute('data-cat') === c) &&
          (st === 'all' || (st === 'dev' ? on(r, 'dev') : r.getAttribute('data-st') === st)) &&
          (cp === 'all' || (cp === 'lordos' && on(r, 'l')) || (cp === 'kwin' && on(r, 'k')) || (cp === 'mutter' && on(r, 'm')) ||
           (cp === 'both' && on(r, 'k') && on(r, 'm')) || (cp === 'gap' && on(r, 'k') && on(r, 'm') && !on(r, 'l'))) &&
          (cf === 'all' || r.getAttribute('data-cf') === cf);
        r.hidden = !ok;
        if (ok) n++;
      });
      wcount.textContent = wtpl.replace('{n}', n).replace('{total}', wpRows.length);
      $('#wp-empty').hidden = n > 0;
      var h = [];
      if (q) h.push('q=' + encodeURIComponent(wq.value.trim()));
      if (c !== 'all') h.push('cat=' + c);
      if (st !== 'all') h.push('st=' + st);
      if (cp !== 'all') h.push('by=' + cp);
      if (cf !== 'all') h.push('cf=' + cf);
      try { history.replaceState(null, '', h.length ? '#' + h.join('&') : location.pathname + location.search); } catch (e) { /* file:// */ }
    };
    // a #q=…&cat=…&st=…&by=…&cf=… address restores the filters (a #p-<id>, #conformance or #proof-<id> one is a plain anchor)
    if (/^#(q|cat|st|by|cf)=/.test(location.hash)) {
      location.hash.slice(1).split('&').forEach(function (kv) {
        var i = kv.indexOf('='), k = kv.slice(0, i), v = decodeURIComponent(kv.slice(i + 1));
        var el = { q: wq, cat: wcat, st: wst, by: wcomp, cf: wcf }[k];
        if (!el) return;
        if (el.tagName === 'SELECT') { if ($$('option', el).some(function (o) { return o.value === v; })) el.value = v; } else el.value = v;
      });
    }
    wq.addEventListener('input', wapply);
    [wcat, wst, wcomp, wcf].forEach(function (s) { if (s) s.addEventListener('change', wapply); });
    if (!location.hash || /^#(q|cat|st|by|cf)=/.test(location.hash)) wapply(); else wcount.textContent = wtpl.replace('{n}', wpRows.length).replace('{total}', wpRows.length);
  }
})();
