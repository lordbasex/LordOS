// search.js — the site search, loaded the first time it is opened (the magnifier in the header,
// Ctrl/⌘ K or "/"). Spotlight-like: a dialog with one field, suggested topics while it is empty, and
// results from /search-<lang>.json (pages, features, guides, questions, apps, legal pages), all
// matched in the browser: nothing is sent anywhere.
(function () {
  var d = document, dlg = d.getElementById('search'), box = d.getElementById('search-q'), list = d.getElementById('search-list');
  var sugg = d.getElementById('search-sugg'), none = d.getElementById('search-none'), btn = d.querySelector('.search-btn');
  if (!dlg || !box || !list) return;
  var data = null, items = [], sel = -1;
  var kinds = JSON.parse(dlg.getAttribute('data-kinds') || '{}');
  var fold = function (s) { return String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, ''); };
  var load = function () {
    if (data) return Promise.resolve(data);
    return fetch(btn.getAttribute('data-index')).then(function (r) { return r.json(); }).then(function (j) {
      data = j.map(function (e) { return { k: e[0], t: e[1], u: e[2], x: e[3] || '', ft: fold(e[1]), fx: fold(e[3] || '') }; });
      return data;
    });
  };
  var esc = function (s) { return String(s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); };
  var mark = function (s, words) {
    var out = esc(s);
    words.forEach(function (w) {
      if (w.length < 2) return;
      var f = fold(s), i = f.indexOf(w);
      if (i >= 0) out = esc(s.slice(0, i)) + '<mark>' + esc(s.slice(i, i + w.length)) + '</mark>' + esc(s.slice(i + w.length));
    });
    return out;
  };
  var select = function (i) {
    var rows = list.querySelectorAll('a');
    sel = Math.max(-1, Math.min(i, rows.length - 1));
    for (var n = 0; n < rows.length; n++) rows[n].setAttribute('aria-selected', String(n === sel));
    if (sel >= 0) { rows[sel].scrollIntoView({ block: 'nearest' }); box.setAttribute('aria-activedescendant', rows[sel].id); }
    else box.removeAttribute('aria-activedescendant');
  };
  var run = function () {
    var q = fold(box.value.trim());
    sugg.hidden = !!q;
    if (!q) { list.innerHTML = ''; none.hidden = true; select(-1); return; }
    var words = q.split(/\s+/);
    load().then(function (all) {
      if (fold(box.value.trim()) !== q) return;
      items = all.map(function (e) {
        var score = 0;
        for (var i = 0; i < words.length; i++) {
          var w = words[i];
          if (e.ft.indexOf(w) >= 0) score += e.ft.indexOf(w) === 0 ? 6 : 4;
          else if (e.fx.indexOf(w) >= 0) score += 1;
          else return null;
        }
        return { e: e, s: score };
      }).filter(Boolean).sort(function (a, b) { return b.s - a.s; }).slice(0, 12);
      list.innerHTML = items.map(function (it, i) {
        var e = it.e, snip = e.x ? '<span class="sr-x">' + mark(e.x.slice(0, 140), words) + '</span>' : '';
        return '<li><a id="sr-' + i + '" role="option" aria-selected="false" href="' + esc(e.u) + '"><span class="sr-k">' + esc(kinds[e.k] || e.k) + '</span><span class="sr-t">' + mark(e.t, words) + '</span>' + snip + '</a></li>';
      }).join('');
      none.hidden = items.length > 0;
      none.querySelector('q').textContent = box.value.trim();
      select(items.length ? 0 : -1);
    });
  };
  box.addEventListener('input', run);
  box.addEventListener('keydown', function (e) {
    if (e.key === 'ArrowDown') { e.preventDefault(); select(sel + 1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); select(sel - 1); }
    else if (e.key === 'Enter' && sel >= 0) { e.preventDefault(); list.querySelectorAll('a')[sel].click(); }
  });
  list.addEventListener('click', function (e) { if (e.target.closest('a')) dlg.close(); });
  dlg.addEventListener('click', function (e) { if (e.target === dlg) dlg.close(); });
  window.__lordSearch = {
    open: function () {
      if (typeof dlg.showModal === 'function') dlg.showModal(); else dlg.setAttribute('open', '');
      box.value = ''; run(); box.focus(); load();
    },
  };
})();
