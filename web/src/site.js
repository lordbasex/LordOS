// lordos.org: the only script on the site (CSP script-src 'self', no inline code).
// Every page works without it; it adds copy buttons, the donation amount, the language
// suggestion on the English home page, and the gallery filters and viewer.
(function () {
  'use strict';
  var d = document;
  var $ = function (s, r) { return (r || d).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || d).querySelectorAll(s)); };
  var store = {
    get: function (k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set: function (k, v) { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } }
  };

  // Copy buttons
  $$('[data-copy]').forEach(function (b) {
    if (!navigator.clipboard) return;
    b.hidden = false;
    var label = b.textContent;
    b.addEventListener('click', function () {
      var src = d.getElementById(b.getAttribute('data-copy'));
      navigator.clipboard.writeText(src.textContent.trim()).then(function () {
        b.textContent = b.getAttribute('data-done');
        setTimeout(function () { b.textContent = label; }, 1800);
      });
    });
  });

  // Donations
  var form = $('#donate');
  if (form) {
    var custom = $('#custom-amount');
    var out = $('#pay-amount');
    var radios = $$('input[name="amount"]', form);
    var wrap = custom.closest('.custom');
    var customValue = function () { var v = parseFloat(custom.value); return v > 0 ? Math.round(v * 100) / 100 : 0; };
    var current = function () {
      if (wrap.classList.contains('on')) return customValue();
      var r = radios.filter(function (x) { return x.checked; })[0];
      return r ? +r.value : 0;
    };
    var sync = function () { var a = current(); out.textContent = a ? String(a) : '—'; };
    radios.forEach(function (r) {
      r.addEventListener('change', function () { wrap.classList.remove('on'); sync(); });
    });
    custom.addEventListener('input', function () {
      wrap.classList.add('on');
      radios.forEach(function (r) { r.checked = false; });
      sync();
    });
    form.addEventListener('submit', function (e) {
      e.preventDefault();
      var a = current();
      location.href = 'https://www.paypal.com/donate/?business=paypal%40iperfex.com&currency_code=USD' + (a ? '&amount=' + a : '');
    });
    sync();
  }

  // Suggest Spanish or Portuguese on the first visit to the English home page. Never redirects.
  var bar = $('#lbar');
  if (bar && location.pathname === '/' && !store.get('lordos-lang-seen')) {
    var nl = (navigator.language || '').toLowerCase().slice(0, 2);
    var opt = bar.getAttribute('data-' + nl);
    store.set('lordos-lang-seen', '1');
    if (opt) {
      var o = JSON.parse(opt);
      bar.setAttribute('lang', nl);
      $('p', bar).textContent = o.text;
      var a = $('a', bar); a.textContent = o.go; a.href = o.href; a.hreflang = nl;
      var x = $('button', bar); x.textContent = o.close;
      x.addEventListener('click', function () { bar.hidden = true; });
      bar.hidden = false;
    }
  }

  // Theme toggle: Auto -> Light -> Dark. theme.js already applied the saved choice before paint.
  var sws = $$('.theme-sw');
  if (sws.length) {
    var root = d.documentElement;
    var mq = window.matchMedia ? matchMedia('(prefers-color-scheme: dark)') : null;
    var isDark = function () { var m = root.getAttribute('data-theme'); return m ? m === 'dark' : !!(mq && mq.matches); };
    var paint = function () { sws.forEach(function (b) { b.setAttribute('aria-checked', String(isDark())); }); };
    sws.forEach(function (b) {
      b.addEventListener('click', function () {
        var m = isDark() ? 'light' : 'dark';
        root.setAttribute('data-theme', m);
        store.set('lordos-theme', m);
        paint();
      });
    });
    if (mq && mq.addEventListener) mq.addEventListener('change', paint);
    paint();
  }

  // mobile menu: upgrade the CSS-only toggle (checkbox + labels) to a modal dialog
  var cb = $('#nav-cb'), drawer = $('#drawer');
  if (cb && drawer) {
    var burger = $('.burger'), xbtn = $('.dclose', drawer), panel = $('.dpanel', drawer);
    cb.tabIndex = -1; cb.setAttribute('aria-hidden', 'true');
    [burger, xbtn].forEach(function (b) {
      b.setAttribute('role', 'button'); b.tabIndex = 0; b.removeAttribute('aria-hidden');
      b.setAttribute('aria-label', b.getAttribute('data-label'));
      b.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); b.click(); }
      });
    });
    burger.setAttribute('aria-controls', 'drawer');
    var navFocusables = function () {
      return $$('a[href], summary, [tabindex="0"]', panel).filter(function (x) { return x.offsetParent !== null; });
    };
    var navState = function () {
      var open = cb.checked;
      burger.setAttribute('aria-expanded', String(open));
      if (open) { drawer.setAttribute('role', 'dialog'); drawer.setAttribute('aria-modal', 'true'); }
      else { drawer.removeAttribute('role'); drawer.removeAttribute('aria-modal'); }
      return open;
    };
    cb.addEventListener('change', function () {
      if (navState()) setTimeout(function () { xbtn.focus(); }, 30);
      else burger.focus();
    });
    var navClose = function () { if (cb.checked) { cb.checked = false; navState(); burger.focus(); } };
    drawer.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') { e.preventDefault(); navClose(); return; }
      if (e.key !== 'Tab') return;
      var f = navFocusables(); if (!f.length) return;
      var first = f[0], last = f[f.length - 1];
      if (e.shiftKey && d.activeElement === first) { last.focus(); e.preventDefault(); }
      else if (!e.shiftKey && d.activeElement === last) { first.focus(); e.preventDefault(); }
    });
    $$('a[href]', panel).forEach(function (a) { a.addEventListener('click', function () { cb.checked = false; navState(); }); });
    window.addEventListener('resize', function () { if (cb.checked && innerWidth > 900) { cb.checked = false; navState(); } });
    navState();
  }

  // back to top: shown after one screen while scrolling; on phones it hides again after 2.5 s idle
  var tt = $('.totop');
  if (tt) {
    tt.hidden = false;
    var ttBusy = false, ttIdle = null;
    var ttUpd = function () {
      ttBusy = false;
      var on = scrollY > innerHeight;
      tt.classList.toggle('on', on);
      clearTimeout(ttIdle);
      if (on && innerWidth <= 640) ttIdle = setTimeout(function () { tt.classList.remove('on'); }, 2500);
    };
    addEventListener('scroll', function () { if (!ttBusy) { ttBusy = true; requestAnimationFrame(ttUpd); } }, { passive: true });
    tt.addEventListener('click', function () {
      scrollTo({ top: 0, behavior: reduce ? 'auto' : 'smooth' });
      var h = $('main h1') || $('#main');
      if (h) { h.tabIndex = -1; h.focus({ preventScroll: true }); }
    });
  }

  // language dropdowns: close on Escape or a click outside
  var dds = $$('.lang-dd');
  d.addEventListener('click', function (e) { dds.forEach(function (dd) { if (dd.open && !dd.contains(e.target)) dd.open = false; }); });
  d.addEventListener('keydown', function (e) {
    if (e.key !== 'Escape') return;
    dds.forEach(function (dd) { if (dd.open) { dd.open = false; $('summary', dd).focus(); } });
  });

  // Home hero slideshow: crossfade every 6 s; pauses on hover, focus, hidden tab or the Pause button;
  // off with prefers-reduced-motion (only the first screen shows). Loads each image just before it shows.
  var show = $('#hero-show');
  var reduce = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (show && !reduce) {
    var slides = $$('.slide', show), ctl = $('.hero-ctl', show), pb = $('.hc-play', show);
    var status = $('.hc-status', show), num = $('.hc-n', show), capEl = $('.hc-cap', show);
    var caps = ctl ? JSON.parse(ctl.getAttribute('data-caps')) : [];
    var cur = 0, timer = null, user = false, hover = false, focus = false;
    var load = function (i) {
      $$('img[data-src]', slides[i]).forEach(function (im) {
        im.srcset = im.getAttribute('data-srcset'); im.src = im.getAttribute('data-src');
        im.removeAttribute('data-src'); im.removeAttribute('data-srcset');
      });
    };
    var running = function () { return !user && !hover && !focus && !d.hidden; };
    var go = function (i) {
      var n = slides.length;
      i = (i + n) % n;
      load(i); load((i + 1) % n);
      slides.forEach(function (sl, k) { sl.classList.toggle('on', k === i); if (k === i) sl.removeAttribute('aria-hidden'); else sl.setAttribute('aria-hidden', 'true'); });
      num.textContent = i + 1; capEl.textContent = caps[i] || '';
      cur = i;
    };
    var sync = function () {
      clearInterval(timer); timer = null;
      if (running()) timer = setInterval(function () { go(cur + 1); }, 6000);
      status.setAttribute('aria-live', running() ? 'off' : 'polite');
    };
    if (slides.length > 1 && ctl) {
      load(1);
      show.addEventListener('mouseenter', function () { hover = true; sync(); });
      show.addEventListener('mouseleave', function () { hover = false; sync(); });
      show.addEventListener('focusin', function (e) { focus = e.target !== pb; sync(); });
      show.addEventListener('focusout', function (e) { if (!show.contains(e.relatedTarget)) { focus = false; sync(); } });
      d.addEventListener('visibilitychange', sync);
      pb.addEventListener('click', function () {
        user = !user;
        pb.setAttribute('data-state', user ? 'paused' : 'playing');
        pb.setAttribute('aria-label', pb.getAttribute(user ? 'data-play' : 'data-pause'));
        sync();
      });
      $('.hc-prev', show).addEventListener('click', function () { go(cur - 1); sync(); });
      $('.hc-next', show).addEventListener('click', function () { go(cur + 1); sync(); });
      ctl.addEventListener('keydown', function (e) {
        if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') { go(cur + (e.key === 'ArrowRight' ? 1 : -1)); sync(); e.preventDefault(); }
      });
      sync();
    }
  }

  // Paper: highlight the section being read in the table of contents
  var tocLinks = $$('.toc a');
  if (tocLinks.length && 'IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (es) {
      es.forEach(function (en) {
        if (!en.isIntersecting) return;
        tocLinks.forEach(function (a) {
          if (a.getAttribute('href') === '#' + en.target.id) a.setAttribute('aria-current', 'true'); else a.removeAttribute('aria-current');
        });
      });
    }, { rootMargin: '0px 0px -70% 0px' });
    $$('.prose h2[id], .prose h3[id]').forEach(function (h) { io.observe(h); });
  }

  // Paper: a link to a release's notes (or a heading inside them) opens its collapsed <details>
  var openChl = function () {
    var t1 = location.hash && d.getElementById(decodeURIComponent(location.hash.slice(1)));
    var dt1 = t1 && t1.closest && t1.closest('details.chl');
    if (dt1 && !dt1.open) { dt1.open = true; t1.scrollIntoView(); }
  };
  if ($('.chlog')) { openChl(); window.addEventListener('hashchange', openChl); }

  // FAQ search (the category chips are plain anchors)
  var fq = $('#faq-q');
  if (fq) {
    var secs = $$('.faq-sec');
    var norm = function (s) { return s.toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, ''); };
    fq.addEventListener('input', function () {
      var q = norm(fq.value.trim()), any = false;
      secs.forEach(function (sec) {
        var n = 0;
        $$('details', sec).forEach(function (dt) {
          var hit = !q || norm(dt.textContent).indexOf(q) >= 0;
          dt.hidden = !hit;
          if (hit) n++;
          if (q) dt.open = hit;
        });
        sec.hidden = n === 0;
        if (n) any = true;
      });
      $('#faq-empty').hidden = any;
    });
    if (location.hash) { var t0 = d.getElementById(location.hash.slice(1)); if (t0 && t0.tagName === 'DETAILS') t0.open = true; }
  }

  // Filters (gallery: category + appearance; apps: category). Items carry data-item.
  var items = $$('[data-item]');
  var more = $('#g-more');
  var PAGE = 48;
  var state = { cat: 'all', theme: 'all', limit: PAGE };
  var empty = $('#g-empty');
  var apply = function () {
    var shown = 0, matching = 0;
    items.forEach(function (f) {
      var t = f.getAttribute('data-theme');
      var ok = (state.cat === 'all' || f.getAttribute('data-cat') === state.cat) &&
               (state.theme === 'all' || !t || t === state.theme);
      if (ok) matching++;
      if (ok && more && shown >= state.limit) ok = false;
      if (ok) shown++;
      f.hidden = !ok;
    });
    if (empty) empty.hidden = matching > 0;
    if (more) more.hidden = shown >= matching;
  };
  var group = function (sel, key) {
    var btns = $$(sel);
    btns.forEach(function (b) {
      b.addEventListener('click', function () {
        state[key] = b.getAttribute('data-' + key);
        state.limit = PAGE;
        btns.forEach(function (x) { x.setAttribute('aria-pressed', String(x === b)); });
        apply();
      });
    });
  };
  if (items.length) {
    group('.g-cats .g-chip', 'cat');
    group('.g-themes .g-chip', 'theme');
    if (more) $('button', more).addEventListener('click', function () { state.limit += PAGE; apply(); });
    apply();
  }

  // Lightbox over every visible screenshot link (.g-card)
  var lb = $('#lightbox');
  if (!lb || typeof lb.showModal !== 'function') return; // the links still open the full image
  var img = $('#lb-img'), cap = $('#lb-cap'), pos = $('#lb-pos');
  var list = [], idx = 0, opener = null;
  var show = function (i) {
    var n = list.length;
    idx = (i + n) % n;
    var a = list[idx];
    img.src = a.getAttribute('href');
    img.width = +a.getAttribute('data-w'); img.height = +a.getAttribute('data-h');
    img.alt = a.getAttribute('data-cap');
    cap.textContent = a.getAttribute('data-cap');
    pos.textContent = (idx + 1) + ' / ' + n;
    lb.setAttribute('aria-label', cap.textContent);
  };
  d.addEventListener('click', function (e) {
    var a = e.target.closest && e.target.closest('a.g-card');
    if (!a || e.metaKey || e.ctrlKey || e.shiftKey || e.button) return;
    e.preventDefault();
    list = $$('a.g-card').filter(function (x) { return x.offsetParent !== null; });
    opener = a;
    show(list.indexOf(a));
    lb.showModal();
  });
  lb.addEventListener('click', function (e) {
    var b = e.target.closest('[data-lb]');
    if (b) {
      var k = b.getAttribute('data-lb');
      if (k === 'close') lb.close(); else show(idx + (k === 'next' ? 1 : -1));
    } else if (e.target === lb) {
      lb.close();
    }
  });
  lb.addEventListener('keydown', function (e) {
    if (e.key === 'ArrowRight') { show(idx + 1); e.preventDefault(); }
    else if (e.key === 'ArrowLeft') { show(idx - 1); e.preventDefault(); }
  });
  lb.addEventListener('close', function () {
    var a = list[idx] || opener;
    if (a) a.focus();
  });
})();
