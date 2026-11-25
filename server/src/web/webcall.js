(function (ev) {
  // webcall.js — what a web app's calls do, for its log (docs/WEBAPPS.md, "Calls"; webcall.h). lord-web adds it at
  // document start in the app's main frame, in the page's own world (where the page's RTCPeerConnection and
  // getUserMedia are), called with an event name random per process. It never adds an API the engine lacks and
  // changes nothing the page sees: the same constructor and methods, which still print as native code, the same
  // results and errors. It only watches, and tells lord-web's bridge (another world, the only one with a way to
  // lord-web) a closed set of facts through that event: states, codecs, sizes, candidate kinds and error names.
  // Never SDP, candidates, addresses, ids or anything the people in the call say.
  'use strict';
  var win = window;
  if (!ev || win.top !== win) return;
  var doc = win.document;
  var dispatch = EventTarget.prototype.dispatchEvent;
  var addL = EventTarget.prototype.addEventListener;
  var CE = win.CustomEvent;
  var stringify = JSON.stringify;
  var fnToString = Function.prototype.toString;
  var natives = new WeakMap();
  var setTimeout_ = win.setTimeout.bind(win);
  var Reflect_ = Reflect;

  function send(o) {
    try { dispatch.call(doc, new CE(ev, { detail: stringify(o) })); } catch (e) { /* nothing to tell */ }
  }

  var toStr = {
    toString() {
      if (natives.has(this)) return natives.get(this);
      return fnToString.call(this);
    }
  }.toString;
  natives.set(toStr, 'function toString() { [native code] }');
  Object.defineProperty(Function.prototype, 'toString', { value: toStr, writable: true, configurable: true, enumerable: false });
  // f prints as orig did (native code, with its own name)
  function like(f, orig) {
    var text;
    try { text = fnToString.call(orig); } catch (e) { text = 'function () { [native code] }'; }
    natives.set(f, text);
    try { Object.defineProperty(f, 'length', { value: orig.length, configurable: true }); } catch (e) { /* stays */ }
    return f;
  }

  // what the page has, once, when its document starts: what a messaging app checks before it offers calls
  function env() {
    var md = win.navigator && win.navigator.mediaDevices;
    send({ k: 'env', coi: win.crossOriginIsolated === true, sab: typeof win.SharedArrayBuffer === 'function',
           atomics: typeof win.Atomics === 'object', rtc: typeof win.RTCPeerConnection === 'function',
           gum: !!(md && typeof md.getUserMedia === 'function') });
  }

  function errName(e) { return e && typeof e.name === 'string' ? e.name : 'Error'; }

  // the page's promise as it was, with its failure written down on the way
  function watchPromise(p, op, id) {
    if (!p || typeof p.then !== 'function') return p;
    p.then(null, function (e) { send({ k: 'error', id: id, op: op, name: errName(e) }); });
    return p;
  }

  var PC = win.RTCPeerConnection;
  if (typeof PC === 'function') {
    var ids = new WeakMap();
    var next = 1;
    var getStats = PC.prototype.getStats;
    var stateKeys = { icegatheringstatechange: ['ice-gathering', 'iceGatheringState'],
                      iceconnectionstatechange: ['ice-connection', 'iceConnectionState'],
                      connectionstatechange: ['connection', 'connectionState'],
                      signalingstatechange: ['signaling', 'signalingState'] };

    function stats(pc, id) {
      try {
        getStats.call(pc).then(function (report) {
          var codecs = {}, seen = {}, pair = null, locals = {}, remotes = {};
          report.forEach(function (s) {
            if (s.type === 'codec') codecs[s.id] = s.mimeType;
            else if (s.type === 'local-candidate') locals[s.id] = s;
            else if (s.type === 'remote-candidate') remotes[s.id] = s;
            else if (s.type === 'candidate-pair' && (s.nominated || s.selected) && s.state === 'succeeded') pair = s;
          });
          report.forEach(function (s) {
            var dir = s.type === 'inbound-rtp' ? 'in' : s.type === 'outbound-rtp' ? 'out' : null;
            if (!dir || (s.kind !== 'audio' && s.kind !== 'video') || seen[dir + s.kind]) return;
            seen[dir + s.kind] = true;
            var o = { k: 'media', id: id, dir: dir, kind: s.kind };
            if (codecs[s.codecId]) o.codec = codecs[s.codecId];
            if (s.kind === 'video' && s.frameWidth && s.frameHeight) {
              o.w = s.frameWidth | 0; o.h = s.frameHeight | 0; o.fps = Math.round(s.framesPerSecond || 0);
            }
            send(o);
          });
          if (pair && locals[pair.localCandidateId] && remotes[pair.remoteCandidateId])
            send({ k: 'pair', id: id, local: locals[pair.localCandidateId].candidateType,
                   remote: remotes[pair.remoteCandidateId].candidateType, proto: locals[pair.localCandidateId].protocol });
        }, function (e) { send({ k: 'error', id: id, op: 'getStats', name: errName(e) }); });
      } catch (e) { /* closed meanwhile */ }
    }

    function track(pc) {
      if (ids.has(pc)) return;
      var id = next++;
      ids.set(pc, id);
      send({ k: 'pc', id: id, ev: 'created' });
      var timer = 0;
      Object.keys(stateKeys).forEach(function (type) {
        addL.call(pc, type, function () {
          var st = pc[stateKeys[type][1]];
          send({ k: 'state', id: id, what: stateKeys[type][0], state: st });
          if (type === 'connectionstatechange' && st === 'connected') {
            // once a second after connecting (codecs and sizes settle), then every five minutes while it lasts
            setTimeout_(function () { stats(pc, id); }, 1000);
            if (!timer) timer = win.setInterval(function () {
              if (pc.connectionState === 'connected') stats(pc, id);
            }, 300000);
          }
          if (type === 'connectionstatechange' && (st === 'failed' || st === 'closed')) {
            if (st === 'failed') stats(pc, id);
            if (timer) { win.clearInterval(timer); timer = 0; }
          }
        });
      });
    }

    var Wrapped = new Proxy(PC, {
      construct: function (target, args, newTarget) {
        var pc;
        try { pc = Reflect_.construct(target, args, newTarget === Wrapped ? target : newTarget); }
        catch (e) { send({ k: 'error', id: 0, op: 'RTCPeerConnection', name: errName(e) }); throw e; }
        track(pc);
        return pc;
      }
    });
    natives.set(Wrapped, fnToString.call(PC));
    Object.defineProperty(PC.prototype, 'constructor', { value: Wrapped, writable: true, configurable: true, enumerable: false });
    Object.defineProperty(win, 'RTCPeerConnection', { value: Wrapped, writable: true, configurable: true, enumerable: false });

    ['createOffer', 'createAnswer', 'setLocalDescription', 'setRemoteDescription', 'addIceCandidate'].forEach(function (op) {
      var orig = PC.prototype[op];
      if (typeof orig !== 'function') return;
      // a method (no prototype, not a constructor), as the engine's own
      var w = {
        [op]() {
          var p = Reflect_.apply(orig, this, arguments);
          return watchPromise(p, op, ids.get(this) || 0);
        }
      }[op];
      Object.defineProperty(PC.prototype, op, { value: like(w, orig), writable: true, configurable: true, enumerable: true });
    });
    var close = PC.prototype.close;
    if (typeof close === 'function') {
      var wclose = { close() {
        var id = ids.get(this);
        if (id && this.connectionState !== 'closed') send({ k: 'pc', id: id, ev: 'closed' });
        return Reflect_.apply(close, this, arguments);
      } }.close;
      Object.defineProperty(PC.prototype, 'close', { value: like(wclose, close), writable: true, configurable: true, enumerable: true });
    }
  }

  var MD = win.MediaDevices;
  ['getUserMedia', 'getDisplayMedia'].forEach(function (op) {
    var orig = MD && MD.prototype[op];
    if (typeof orig !== 'function') return;
    var w = {
      [op]() {
        var c = arguments[0];
        var a = !!(c && c.audio), v = !!(c && c.video);
        if (op === 'getDisplayMedia' && !c) v = true;
        var p = Reflect_.apply(orig, this, arguments);
        if (p && typeof p.then === 'function') p.then(function (stream) {
          var labels = [];
          try { stream.getTracks().forEach(function (t) { labels.push(String(t.label || '')); }); } catch (e) { /* no tracks */ }
          send({ k: 'gum', op: op, audio: a, video: v, result: 'granted', labels: labels });
        }, function (e) {
          send({ k: 'gum', op: op, audio: a, video: v, result: 'failed', name: errName(e) });
        });
        return p;
      }
    }[op];
    Object.defineProperty(MD.prototype, op, { value: like(w, orig), writable: true, configurable: true, enumerable: true });
  });

  // after the bridge in lord-web's world is listening (the worlds' scripts start in their own order)
  setTimeout_(env, 0);
})
