(function (key, dark) {
  // webscheme.js — the page's matchMedia answers prefers-color-scheme with the desktop's scheme (docs/WEBAPPS.md,
  // "Light and dark"; webscheme.h). lord-web adds it at document start in every frame, in the page's own world (the
  // page's scripts must see it), calling it with a hook name random per process and the scheme ("dark": true).
  //
  // WPE WebKit 2.48 always answers light. A query that names prefers-color-scheme is rewritten so the real
  // matchMedia answers it as the desktop would: when the engine's answer and the desktop's differ, every
  // "(prefers-color-scheme: dark)" becomes "(prefers-color-scheme: light)" and the other way round, and the rest
  // (not, and, only, commas, other features) stays the real matchMedia's. The page gets a MediaQueryList of its own
  // (instanceof MediaQueryList, an EventTarget) whose media is the original query's and which sends 'change' when
  // the desktop's scheme flips, as well as when the real list changes. Any other query gets the real list untouched.
  //
  // window[key](dark) is how lord-web tells it a new scheme (webscheme_call): it passes it on to same-origin child
  // frames. Nothing here reaches lord-web: the page may call the hook, which changes only what the page itself sees.
  'use strict';
  var win = window;
  var real = win.matchMedia;
  if (typeof real !== 'function' || Object.prototype.hasOwnProperty.call(win, key)) return;
  var ET = win.EventTarget;
  var MQL = win.MediaQueryList;
  var MQLE = win.MediaQueryListEvent;
  var addL = ET.prototype.addEventListener;
  var removeL = ET.prototype.removeEventListener;
  var dispatch = ET.prototype.dispatchEvent;
  var names = /prefers-color-scheme/i;
  var feature = /\(\s*prefers-color-scheme\s*:\s*(light|dark)\s*\)/gi;
  dark = !!dark;
  var engineDark = false;
  try { engineDark = !!real.call(win, '(prefers-color-scheme: dark)').matches; } catch (e) { /* stays light */ }
  var gen = 0;
  var live = new Set();               // the lists with 'change' listeners: kept while they have any, as browsers do

  function rewrite(q) {
    if (dark === engineDark) return q;
    return q.replace(feature, function (m, v) { return '(prefers-color-scheme: ' + (v.toLowerCase() === 'dark' ? 'light' : 'dark') + ')'; });
  }

  function hidden(obj, name, value) {
    Object.defineProperty(obj, name, { value: value, writable: true, enumerable: false, configurable: true });
  }

  function makeEvent(media, matches) {
    if (typeof MQLE === 'function') {
      try { return new MQLE('change', { media: media, matches: matches }); } catch (e) { /* below */ }
    }
    var ev = new win.Event('change');
    Object.defineProperty(ev, 'media', { value: media, enumerable: true });
    Object.defineProperty(ev, 'matches', { value: matches, enumerable: true });
    return ev;
  }

  function wrap(text) {
    var list = new ET();
    Object.setPrototypeOf(list, MQL ? MQL.prototype : ET.prototype);
    var media = text;
    try { media = real.call(win, text).media; } catch (e) { /* the text itself */ }
    var inner = null, innerGen = -1, handler = null, last;
    var keys = new Set();             // listener and capture of each 'change' listener the page added
    function onInner() { fire(); }
    function current() {
      if (innerGen !== gen) {
        if (inner && keys.size) removeL.call(inner, 'change', onInner);
        inner = real.call(win, rewrite(text));
        innerGen = gen;
        if (keys.size) addL.call(inner, 'change', onInner);
      }
      return inner;
    }
    function fire() {
      var m = !!current().matches;
      if (m === last) return;
      last = m;
      dispatch.call(list, makeEvent(media, m));
    }
    function track(add, type, l, opts) {
      if (String(type) !== 'change' || l == null) return;
      var k = (typeof opts === 'boolean' ? opts : !!(opts && opts.capture));
      var id = keys.size ? Array.from(keys).find(function (x) { return x.l === l && x.c === k; }) : undefined;
      if (add && !id) {
        keys.add({ l: l, c: k });
        if (keys.size === 1) { last = !!current().matches; addL.call(current(), 'change', onInner); live.add(fire); }
      } else if (!add && id) {
        keys.delete(id);
        if (!keys.size) { removeL.call(current(), 'change', onInner); live.delete(fire); }
      }
    }
    Object.defineProperty(list, 'matches', { get: function () { return !!current().matches; }, enumerable: false, configurable: true });
    Object.defineProperty(list, 'media', { get: function () { return media; }, enumerable: false, configurable: true });
    function onchange(e) { if (typeof handler === 'function') handler.call(list, e); }
    Object.defineProperty(list, 'onchange', {
      get: function () { return handler; },
      set: function (v) {
        var f = typeof v === 'function' ? v : null;
        if (f && !handler) { addL.call(list, 'change', onchange); track(true, 'change', onchange, false); }
        if (!f && handler) { removeL.call(list, 'change', onchange); track(false, 'change', onchange, false); }
        handler = f;
      },
      enumerable: false, configurable: true,
    });
    hidden(list, 'addEventListener', function addEventListener(type, l, opts) {
      addL.call(list, type, l, opts);
      track(true, type, l, opts);
    });
    hidden(list, 'removeEventListener', function removeEventListener(type, l, opts) {
      removeL.call(list, type, l, opts);
      track(false, type, l, opts);
    });
    hidden(list, 'addListener', function addListener(l) { list.addEventListener('change', l); });
    hidden(list, 'removeListener', function removeListener(l) { list.removeEventListener('change', l); });
    return list;
  }

  function matchMedia(query) {
    if (arguments.length < 1) return real.apply(win, arguments);
    var text = String(query);
    if (!names.test(text)) return real.call(win, query);
    return wrap(text);
  }

  function setScheme(d) {
    d = !!d;
    if (d !== dark) {
      dark = d;
      gen++;
      Array.from(live).forEach(function (f) { f(); });
    }
    var frames = win.frames;
    for (var i = 0; frames && i < frames.length; i++) {
      try {
        var f = frames[i][key];
        if (typeof f === 'function') f(d);
      } catch (e) { /* another origin: it gets the new scheme with its next load */ }
    }
  }

  var was = Object.getOwnPropertyDescriptor(win, 'matchMedia');
  Object.defineProperty(win, 'matchMedia', { value: matchMedia, writable: true, enumerable: was ? !!was.enumerable : true, configurable: true });
  Object.defineProperty(win, key, { value: setScheme, writable: false, enumerable: false, configurable: false });
})
