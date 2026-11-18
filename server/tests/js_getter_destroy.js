// js_getter_destroy.js — the theme tests/js_getter_destroy.sh loads: a getter, an array index or a key's toString
// destroys the node while lord.setProp('wave'), lord.setStyle('gradient') or setProp's key reads the caller's object
// (Claude Security, 0.8.9). lord-server must look the node up again after such a read, never write through the freed one.
const spray = () => { const a = []; for (let i = 0; i < 2000; i++) a.push(new ArrayBuffer(88), 'x'.repeat(200)); return a; };
let keep = [];
const w = lord.createNode('wave');
lord.setProp(w, 'wave', { get lines() { lord.destroy(w); keep.push(spray()); return 4; }, gain: 1, colors: [0xff00ff, 0x00ffff, 0xffffff] });
const v = lord.createNode('view');
const g = [180, 0, 0];
Object.defineProperty(g, 1, { get() { lord.destroy(v); keep.push(spray()); return 0xff0000; } });
lord.setStyle(v, 'gradient', g);
// the key's toString destroys the node before it is used
const v2 = lord.createNode('view');
lord.setProp(v2, { toString() { lord.destroy(v2); keep.push(spray()); return 'protected'; } }, true);
console.log('UAF-TEST-DONE');
