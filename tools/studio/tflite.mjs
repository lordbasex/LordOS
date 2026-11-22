// tflite.mjs — a minimal, strict reader of TensorFlow Lite model files (FlatBuffers), for tflite2lnn.mjs.
//
// It reads only what LordOS Studio's converter needs: operator codes, the first subgraph's tensors, inputs, outputs
// and operators (with their builtin options as raw tables), and the buffers. Every offset is bounds-checked; a file
// that points outside itself is refused. The schema is TensorFlow's schema.fbs (Apache-2.0); field numbers are
// written here by hand, no code is copied.

export const OP = {
  ADD: 0, AVERAGE_POOL_2D: 1, CONCATENATION: 2, CONV_2D: 3, DEPTHWISE_CONV_2D: 4, DEQUANTIZE: 6, LOGISTIC: 14,
  MAX_POOL_2D: 17, MUL: 18, RELU: 19, RELU6: 21, RESHAPE: 22, RESIZE_BILINEAR: 23, SOFTMAX: 25, PAD: 34, MEAN: 40,
  SUB: 41, TRANSPOSE_CONV: 67, HARD_SWISH: 117, CUSTOM: 32,
};
export const OP_NAME = Object.fromEntries(Object.entries(OP).map(([k, v]) => [v, k]));
export const TT = { FLOAT32: 0, FLOAT16: 1, INT32: 2, UINT8: 3, INT64: 4 };

class Reader {
  constructor(buf) { this.b = buf; this.n = buf.length; }
  need(off, len) { if (off < 0 || len < 0 || off + len > this.n) throw new Error(`tflite: offset ${off}+${len} outside the file`); }
  u8(o) { this.need(o, 1); return this.b.readUInt8(o); }
  u16(o) { this.need(o, 2); return this.b.readUInt16LE(o); }
  i32(o) { this.need(o, 4); return this.b.readInt32LE(o); }
  u32(o) { this.need(o, 4); return this.b.readUInt32LE(o); }
  u64(o) { this.need(o, 8); return Number(this.b.readBigUInt64LE(o)); }
  // a table at absolute offset t; returns the absolute offset of field i, or 0 when absent
  field(t, i) {
    const vt = t - this.i32(t);
    const vlen = this.u16(vt);
    if (vlen < 4 || vlen > 1024) throw new Error('tflite: bad vtable');
    const slot = 4 + 2 * i;
    if (slot + 2 > vlen) return 0;
    const fo = this.u16(vt + slot);
    return fo ? t + fo : 0;
  }
  ref(o) { return o + this.u32(o); }                 // follow a uoffset
  table(t, i) { const f = this.field(t, i); return f ? this.ref(f) : 0; }
  vec(t, i) {                                         // [abs offset of the first element, count]
    const f = this.field(t, i);
    if (!f) return [0, 0];
    const v = this.ref(f);
    const n = this.u32(v);
    if (n > 1 << 26) throw new Error('tflite: vector too long');
    return [v + 4, n];
  }
  ints(t, i) { const [p, n] = this.vec(t, i); this.need(p, n * 4); const a = []; for (let k = 0; k < n; k++) a.push(this.i32(p + 4 * k)); return a; }
  tables(t, i) { const [p, n] = this.vec(t, i); this.need(p, n * 4); const a = []; for (let k = 0; k < n; k++) a.push(this.ref(p + 4 * k)); return a; }
  str(t, i) { const [p, n] = this.vec(t, i); this.need(p, n); return this.b.toString('utf8', p, p + n); }
  bytes(t, i) { const [p, n] = this.vec(t, i); this.need(p, n); return this.b.subarray(p, p + n); }
  sI32(t, i, d = 0) { const f = this.field(t, i); return f ? this.i32(f) : d; }
  sU8(t, i, d = 0) { const f = this.field(t, i); return f ? this.u8(f) : d; }
  sBool(t, i) { return this.sU8(t, i, 0) !== 0; }
  sU64(t, i) { const f = this.field(t, i); return f ? this.u64(f) : 0; }
}

// Parses a .tflite file into { opcodes, tensors, inputs, outputs, ops, buffers, r } (r: the reader, for options).
export function readTflite(buf) {
  if (buf.length < 8 || buf.toString('latin1', 4, 8) !== 'TFL3') throw new Error('tflite: not a TFL3 file');
  const r = new Reader(buf);
  const model = r.ref(0);
  const opcodes = r.tables(model, 1).map((t) => {
    const dep = r.sU8(t, 0, 0), code = r.sI32(t, 3, 0);
    return { code: Math.max(dep, code), custom: r.field(t, 1) ? r.str(t, 1) : '' };
  });
  const subgraphs = r.tables(model, 2);
  if (subgraphs.length !== 1) throw new Error(`tflite: ${subgraphs.length} subgraphs (one expected)`);
  const sg = subgraphs[0];
  const buffers = r.tables(model, 4).map((t) => {
    const off = r.sU64(t, 1), size = r.sU64(t, 2);
    if (off) { r.need(off, size); return buf.subarray(off, off + size); }
    return r.field(t, 0) ? r.bytes(t, 0) : Buffer.alloc(0);
  });
  const tensors = r.tables(sg, 0).map((t) => ({
    shape: r.ints(t, 0), type: r.sU8(t, 1, 0), buffer: r.sI32(t, 2, 0), name: r.field(t, 3) ? r.str(t, 3) : '',
  }));
  const ops = r.tables(sg, 3).map((t) => {
    const oc = opcodes[r.sI32(t, 0, 0)];
    if (!oc) throw new Error('tflite: bad opcode index');
    return { code: oc.code, custom: oc.custom, inputs: r.ints(t, 1), outputs: r.ints(t, 2), optType: r.sU8(t, 3, 0), opt: r.table(t, 4),
             customOptions: r.field(t, 5) ? Buffer.from(r.bytes(t, 5)) : null };
  });
  return { opcodes, tensors, inputs: r.ints(sg, 1), outputs: r.ints(sg, 2), ops, buffers, r };
}
