(function (d) {
  // webua.js — navigator.userAgentData and navigator.vendor as Chrome has them, for a profile whose Browser identity
  // is "chrome" (docs/WEBAPPS.md, "Browser identity"; webua.h). lord-web adds it at document start in every frame, in
  // the page's own world (the page's scripts must see it), with no privileges and no message handler: it only answers
  // the page with the values lord-web built in C from the same table as the User-Agent string and the Sec-CH-UA
  // headers, so the three never disagree. Nothing goes from the page to lord-web.
  //
  // As in Chrome: userAgentData only in a secure context; NavigatorUAData is an interface the page can't construct;
  // brands is one frozen list; getHighEntropyValues(hints) resolves to brands, mobile and platform plus the hints
  // asked for, in a dictionary's (alphabetical) order; toJSON() is brands, mobile, platform. Functions read as native
  // code, as Chrome's do.
  'use strict';
  var win = window;
  var NavP = win.Navigator && win.Navigator.prototype;
  if (!NavP || !d) return;
  var defineProperty = Object.defineProperty;
  var freeze = Object.freeze;
  var create = Object.create;
  var P = win.Promise;
  var TE = win.TypeError;
  var fnToString = Function.prototype.toString;
  var natives = new WeakMap();

  // our functions print as native code (the page may print them); everything else as before
  var toStr = {
    toString() {
      if (natives.has(this)) return 'function ' + natives.get(this) + '() { [native code] }';
      return fnToString.call(this);
    }
  }.toString;
  natives.set(toStr, 'toString');
  defineProperty(Function.prototype, 'toString', { value: toStr, writable: true, configurable: true, enumerable: false });
  function native(f, name) { natives.set(f, name); return f; }

  function brandList(list) {
    var out = [];
    for (var i = 0; i < list.length; i++) out.push({ brand: list[i].brand, version: list[i].version });
    return out;
  }
  var brands = freeze(brandList(d.brands).map(function (b) { return freeze(b); }));
  var high = {
    architecture: function () { return d.architecture; },
    bitness: function () { return d.bitness; },
    formFactors: function () { return d.formFactors.slice(); },
    fullVersionList: function () { return brandList(d.fullVersionList); },
    model: function () { return d.model; },
    platformVersion: function () { return d.platformVersion; },
    uaFullVersion: function () { return d.uaFullVersion; },
    wow64: function () { return d.wow64; }
  };
  var order = ['architecture', 'bitness', 'brands', 'formFactors', 'fullVersionList', 'mobile', 'model', 'platform',
               'platformVersion', 'uaFullVersion', 'wow64'];

  var NavigatorUAData = native({
    NavigatorUAData: function () { throw new TE('Illegal constructor'); }
  }.NavigatorUAData, 'NavigatorUAData');
  var proto = NavigatorUAData.prototype;
  var api = {
    get brands() { if (this !== uad) throw new TE('Illegal invocation'); return brands; },
    get mobile() { if (this !== uad) throw new TE('Illegal invocation'); return false; },
    get platform() { if (this !== uad) throw new TE('Illegal invocation'); return d.platform; },
    getHighEntropyValues(hints) {
      if (this !== uad) return P.reject(new TE('Illegal invocation'));
      if (arguments.length < 1)
        return P.reject(new TE("Failed to execute 'getHighEntropyValues' on 'NavigatorUAData': 1 argument required, but only 0 present."));
      var asked = {};
      try {
        if (hints === null || hints === undefined || typeof hints[Symbol.iterator] !== 'function') throw 0;
        var it = hints[Symbol.iterator]();
        for (var s = it.next(); !s.done; s = it.next()) asked[String(s.value)] = true;
      } catch (e) {
        return P.reject(new TE("Failed to execute 'getHighEntropyValues' on 'NavigatorUAData': The provided value cannot be converted to a sequence."));
      }
      var out = {};
      for (var i = 0; i < order.length; i++) {
        var k = order[i];
        if (k === 'brands') out.brands = brandList(d.brands);
        else if (k === 'mobile') out.mobile = false;
        else if (k === 'platform') out.platform = d.platform;
        else if (asked[k]) out[k] = high[k]();
      }
      return P.resolve(out);
    },
    toJSON() {
      if (this !== uad) throw new TE('Illegal invocation');
      return { brands: brandList(d.brands), mobile: false, platform: d.platform };
    }
  };
  ['brands', 'mobile', 'platform'].forEach(function (k) {
    var g = Object.getOwnPropertyDescriptor(api, k).get;
    defineProperty(proto, k, { get: native(g, 'get ' + k), enumerable: true, configurable: true });
  });
  ['getHighEntropyValues', 'toJSON'].forEach(function (k) {
    defineProperty(proto, k, { value: native(api[k], k), writable: true, enumerable: true, configurable: true });
  });
  defineProperty(proto, Symbol.toStringTag, { value: 'NavigatorUAData', configurable: true });
  var uad = create(proto);

  if (win.isSecureContext) {
    defineProperty(win, 'NavigatorUAData', { value: NavigatorUAData, writable: true, configurable: true, enumerable: false });
    var getUad = { get userAgentData() { return uad; } };
    defineProperty(NavP, 'userAgentData', {
      get: native(Object.getOwnPropertyDescriptor(getUad, 'userAgentData').get, 'get userAgentData'),
      enumerable: true, configurable: true
    });
  }
  var getVendor = { get vendor() { return d.vendor; } };
  defineProperty(NavP, 'vendor', {
    get: native(Object.getOwnPropertyDescriptor(getVendor, 'vendor').get, 'get vendor'),
    enumerable: true, configurable: true
  });
})
