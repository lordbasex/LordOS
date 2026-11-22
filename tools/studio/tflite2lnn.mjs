// tflite2lnn.mjs — converts LordOS Studio's two MediaPipe models (docs/CAMERA-STUDIO-PLAN.md 4.2) from TensorFlow
// Lite to our own flat weight file (.lnn) that lord-studio's inference code reads (server/src/media/studio/nn.c).
//
//   node tools/studio/tflite2lnn.mjs <model.tflite> <out.lnn> [--onnx <out.onnx>]
//
// It runs once, on the maintainer's machine; the .lnn files are committed (assets/studio/models/) with their SHA-256
// pinned in server/src/media/studio/models.h. It refuses any operator, option or shape outside the small set the
// two models use, so a different model can't slip through. Float16 weights stay float16 in the file (nn.c widens them
// to float32 when it loads them). The output is a derivative of Google's Apache-2.0 weights (THIRD_PARTY.md).
//
// --onnx also writes the same graph as an ONNX model, expressed with ONNX's own attributes (auto_pad SAME_UPPER for
// TFLite's SAME, Resize with half_pixel, ConvTranspose…), so that ONNX Runtime computes reference outputs on the dev
// machine with its own arithmetic (tools/studio/ort-ref.c). ONNX Runtime is never linked by LordOS.
//
// The .lnn format (little-endian, every record 4-byte aligned; nn.h mirrors it):
//   header 64 bytes: "LNN1", u32 version (1), u32 tensors, u32 ops, u32 halfs (f16 values in the data), u32 input,
//                    u32 outputs, u32 output[4], u32 reserved[5]
//   tensors × 32:    u32 kind (1 activation, 2 constant), u32 dims[4] (activations: 1, h, w, c), u32 offset (in
//                    halfs), u32 count, u32 pad
//   ops × 64:        u32 op, i32 in[3] (-1 none), i32 out, i32 p[11]
//   data:            halfs × 2 bytes (float16)
import fs from 'node:fs';
import { readTflite, OP, OP_NAME, TT } from './tflite.mjs';

export const LOP = { CONV: 1, DWCONV: 2, ADD: 3, MUL: 4, MEAN: 5, RESIZE: 6, TCONV: 7, PAD: 8, MAXPOOL: 9, RESHAPE: 10, CONCAT: 11, RELU: 12, HSWISH: 13, SIGMOID: 14 };
export const ACT = { NONE: 0, RELU: 1, HSWISH: 2, SIGMOID: 3 };
const PAD_SAME = 0, PAD_VALID = 1;

function fail(msg) { throw new Error(`tflite2lnn: ${msg}`); }

// shape → [1, h, w, c]
function dims4(shape) {
  if (shape.length < 1 || shape.length > 4 || shape[0] !== 1) fail(`unsupported shape ${JSON.stringify(shape)}`);
  const d = [...shape];
  while (d.length < 4) d.splice(1, 0, 1);
  for (const v of d) if (!Number.isInteger(v) || v < 1 || v > 4096) fail(`bad dimension in ${JSON.stringify(shape)}`);
  return d;
}

export function convert(buf) {
  const m = readTflite(buf);
  const r = m.r;
  const T = m.tensors;
  const constF16 = new Map();          // f32 tensor index (after DEQUANTIZE) → { dims, data: Buffer of f16 }
  const intConst = (i) => {
    const t = T[i];
    if (!t || t.type !== TT.INT32) fail(`tensor ${i} is not an int32 constant`);
    const b = m.buffers[t.buffer];
    if (!b || b.length !== t.shape.reduce((a, v) => a * v, 1) * 4) fail(`int32 constant ${i} has no data`);
    const out = [];
    for (let k = 0; k < b.length; k += 4) out.push(b.readInt32LE(k));
    return out;
  };
  // 1. constants: a FLOAT16 buffer turned into FLOAT32 by DEQUANTIZE
  const rest = [];
  for (const op of m.ops) {
    if (op.code === OP.DEQUANTIZE) {
      const src = T[op.inputs[0]], dst = op.outputs[0];
      const b = m.buffers[src.buffer];
      const n = src.shape.reduce((a, v) => a * v, 1);
      if (src.type !== TT.FLOAT16 || !b || b.length !== n * 2) fail('DEQUANTIZE of something other than float16 weights');
      if (JSON.stringify(src.shape) !== JSON.stringify(T[dst].shape)) fail('DEQUANTIZE changes the shape');
      constF16.set(dst, { dims: src.shape, data: b });
    } else rest.push(op);
  }
  for (const t of T) if (t.type === TT.FLOAT32 && m.buffers[t.buffer] && m.buffers[t.buffer].length) fail(`float32 weights in "${t.name}" (only float16 models)`);
  const isConst = (i) => constF16.has(i);
  const cdims = (i) => constF16.get(i).dims;
  const sh = (i) => dims4(T[i].shape);

  // 2. operators
  const ops = [];
  for (const op of rest) {
    const o = { op: 0, in: [-1, -1, -1], out: op.outputs[0], p: new Array(11).fill(0) };
    if (op.outputs.length !== 1) fail(`${OP_NAME[op.code]} with ${op.outputs.length} outputs`);
    const x = op.inputs[0];
    if (isConst(x)) fail(`${OP_NAME[op.code]} on a constant`);
    const opt = op.opt;
    const actOf = (f) => { const a = f ? r.sU8(opt, f.idx, 0) : 0; if (a !== 0 && a !== 1) fail(`fused activation ${a}`); return a === 1 ? ACT.RELU : ACT.NONE; };
    switch (op.code) {
    case OP.CONV_2D: case OP.DEPTHWISE_CONV_2D: {
      const dw = op.code === OP.DEPTHWISE_CONV_2D;
      const [w, b] = [op.inputs[1], op.inputs[2]];
      if (!isConst(w) || !isConst(b)) fail('conv weights must be constants');
      const wd = cdims(w), bd = cdims(b), xs = sh(x), os = sh(o.out);
      if (wd.length !== 4 || bd.length !== 1) fail('conv weight rank');
      const pad = r.sU8(opt, 0, 0), sw = r.sI32(opt, 1, 1), shh = r.sI32(opt, 2, 1);
      const act = r.sU8(opt, dw ? 4 : 3, 0);
      const dlw = r.sI32(opt, dw ? 5 : 4, 1), dlh = r.sI32(opt, dw ? 6 : 5, 1);
      if (dlw !== 1 || dlh !== 1) fail('dilated convolution');
      if (act !== 0 && act !== 1) fail(`conv activation ${act}`);
      if (pad !== PAD_SAME && pad !== PAD_VALID) fail('padding');
      if (sw < 1 || sw > 2 || shh < 1 || shh > 2) fail('stride');
      if (dw) {
        if (r.sI32(opt, 3, 1) !== 1 || wd[0] !== 1 || wd[3] !== xs[3] || os[3] !== xs[3]) fail('depthwise multiplier must be 1');
      } else if (wd[3] !== xs[3] || wd[0] !== os[3]) fail('conv channels');
      if (bd[0] !== os[3]) fail('bias size');
      if (wd[1] > 7 || wd[2] > 7) fail('kernel larger than 7');
      o.op = dw ? LOP.DWCONV : LOP.CONV;
      o.in = [x, w, b];
      o.p[0] = shh; o.p[1] = sw; o.p[2] = pad; o.p[3] = act === 1 ? ACT.RELU : ACT.NONE;
      break;
    }
    case OP.ADD: case OP.MUL: {
      const [a, b] = op.inputs;
      if (isConst(b)) fail('ADD/MUL with a constant');
      const as = sh(a), bs = sh(b);
      const same = JSON.stringify(as) === JSON.stringify(bs);
      const bcast = op.code === OP.MUL && bs[1] === 1 && bs[2] === 1 && bs[3] === as[3];
      if (!same && !bcast) fail(`${OP_NAME[op.code]} shapes ${as} ${bs}`);
      o.op = op.code === OP.ADD ? LOP.ADD : LOP.MUL;
      o.in = [a, b, -1];
      o.p[3] = actOf(opt ? { idx: 0 } : null);
      break;
    }
    case OP.MEAN: {
      const axes = intConst(op.inputs[1]);
      if (JSON.stringify(axes) !== '[1,2]' || !r.sBool(opt, 0)) fail('MEAN other than a global spatial average with keep_dims');
      o.op = LOP.MEAN; o.in = [x, -1, -1];
      break;
    }
    case OP.RESIZE_BILINEAR: {
      const size = intConst(op.inputs[1]), os = sh(o.out);
      if (size[0] !== os[1] || size[1] !== os[2]) fail('RESIZE size');
      o.op = LOP.RESIZE; o.in = [x, -1, -1];
      o.p[0] = r.sBool(opt, 2) ? 1 : 0; o.p[1] = r.sBool(opt, 3) ? 1 : 0;
      if (o.p[0] && o.p[1]) fail('align_corners with half_pixel_centers');
      break;
    }
    case OP.CUSTOM: {
      if (op.custom !== 'Convolution2DTransposeBias') fail(`custom operator ${op.custom}`);
      // custom options: TfLiteTransposeConvParams { padding (1 same, 2 valid), stride_width, stride_height }
      const [w, b] = [op.inputs[1], op.inputs[2]];
      if (!isConst(w) || !isConst(b)) fail('transposed conv weights must be constants');
      const wd = cdims(w), xs = sh(x), os = sh(o.out);
      if (wd.length !== 4 || wd[3] !== xs[3] || wd[0] !== os[3] || cdims(b)[0] !== os[3]) fail('transposed conv shapes');
      const co = op.customOptions;
      if (!co || co.length !== 12) fail('transposed conv options');
      const padding = co.readInt32LE(0), sw = co.readInt32LE(4), shh = co.readInt32LE(8);
      if (padding !== 1 || sw !== 2 || shh !== 2 || wd[1] !== 2 || wd[2] !== 2) fail('only a 2×2 stride-2 SAME transposed conv');
      if (os[1] !== xs[1] * 2 || os[2] !== xs[2] * 2) fail('transposed conv output size');
      o.op = LOP.TCONV; o.in = [x, w, b];
      o.p[0] = shh; o.p[1] = sw; o.p[2] = PAD_SAME;
      break;
    }
    case OP.PAD: {
      const pads = intConst(op.inputs[1]);
      if (pads.length !== 8 || pads.slice(0, 6).some((v) => v !== 0) || pads[6] < 0 || pads[7] < 0) fail('PAD other than on channels');
      o.op = LOP.PAD; o.in = [x, -1, -1]; o.p[0] = pads[6]; o.p[1] = pads[7];
      if (sh(o.out)[3] !== sh(x)[3] + pads[6] + pads[7]) fail('PAD shape');
      break;
    }
    case OP.MAX_POOL_2D: {
      const pad = r.sU8(opt, 0, 0), sw = r.sI32(opt, 1, 1), shh = r.sI32(opt, 2, 1), fw = r.sI32(opt, 3, 1), fh = r.sI32(opt, 4, 1), act = r.sU8(opt, 5, 0);
      if (act !== 0 || fw > 3 || fh > 3 || sw > 2 || shh > 2) fail('MAX_POOL options');
      o.op = LOP.MAXPOOL; o.in = [x, -1, -1];
      o.p[0] = shh; o.p[1] = sw; o.p[2] = pad; o.p[4] = fh; o.p[5] = fw;
      break;
    }
    case OP.RESHAPE: {
      const n = (s) => s.reduce((a, v) => a * v, 1);
      if (n(T[x].shape) !== n(T[o.out].shape)) fail('RESHAPE size');
      o.op = LOP.RESHAPE; o.in = [x, -1, -1];
      break;
    }
    case OP.CONCATENATION: {
      if (op.inputs.length !== 2) fail('CONCATENATION of more than two');
      const axis = r.sI32(opt, 0, 0), act = r.sU8(opt, 1, 0);
      const s0 = T[op.inputs[0]].shape;
      if (act !== 0 || axis < 1 || axis >= s0.length) fail('CONCATENATION options');
      for (let k = 0; k < axis; k++) if (s0[k] !== 1 || T[op.inputs[1]].shape[k] !== 1) fail('CONCATENATION on an inner axis');
      for (let k = axis + 1; k < s0.length; k++) if (s0[k] !== T[op.inputs[1]].shape[k]) fail('CONCATENATION shapes');
      o.op = LOP.CONCAT; o.in = [op.inputs[0], op.inputs[1], -1];
      break;
    }
    case OP.RELU: o.op = LOP.RELU; o.in = [x, -1, -1]; break;
    case OP.HARD_SWISH: o.op = LOP.HSWISH; o.in = [x, -1, -1]; break;
    case OP.LOGISTIC: o.op = LOP.SIGMOID; o.in = [x, -1, -1]; break;
    default: fail(`operator ${OP_NAME[op.code] || op.code}`);
    }
    sh(o.out);
    ops.push(o);
  }

  // 3. fold an activation into the operator before it when that operator's output feeds only the activation
  const uses = new Map();
  for (const o of ops) for (const i of o.in) if (i >= 0) uses.set(i, (uses.get(i) || 0) + 1);
  for (const i of m.outputs) uses.set(i, (uses.get(i) || 0) + 1000);
  const fusable = new Set([LOP.CONV, LOP.DWCONV, LOP.ADD, LOP.MUL, LOP.TCONV]);
  const actOfOp = { [LOP.RELU]: ACT.RELU, [LOP.HSWISH]: ACT.HSWISH, [LOP.SIGMOID]: ACT.SIGMOID };
  const fused = [];
  for (let k = 0; k < ops.length; k++) {
    const o = ops[k];
    const a = actOfOp[o.op];
    if (a !== undefined) {
      const prev = fused.find((q) => q.out === o.in[0]);
      if (prev && fusable.has(prev.op) && prev.p[3] === ACT.NONE && uses.get(o.in[0]) === 1) { prev.p[3] = a; prev.out = o.out; continue; }
    }
    fused.push(o);
  }

  // 4. renumber: the activations, then the constants in order of first use
  const idx = new Map(), tensors = [], chunks = [];
  let halfs = 0;
  const addAct = (i) => {
    if (idx.has(i)) return idx.get(i);
    if (T[i].type !== TT.FLOAT32) fail(`activation ${i} is not float32`);
    idx.set(i, tensors.length);
    tensors.push({ kind: 1, dims: sh(i), off: 0, count: 0, name: T[i].name });
    return idx.get(i);
  };
  const addConst = (i) => {
    if (idx.has(i)) return idx.get(i);
    const c = constF16.get(i);
    const d = [...c.dims];
    while (d.length < 4) d.unshift(1);
    const count = c.data.length / 2;
    idx.set(i, tensors.length);
    tensors.push({ kind: 2, dims: d, off: halfs, count, name: T[i].name });
    chunks.push(c.data);
    halfs += count;
    return idx.get(i);
  };
  if (m.inputs.length !== 1) fail('one input expected');
  if (m.outputs.length < 1 || m.outputs.length > 4) fail('one to four outputs expected');
  addAct(m.inputs[0]);
  const lops = fused.map((o) => ({
    op: o.op,
    in: o.in.map((i) => (i < 0 ? -1 : isConst(i) ? addConst(i) : addAct(i))),
    out: addAct(o.out),
    p: o.p,
  }));
  const out = m.outputs.map((i) => { if (!idx.has(i)) fail('an output is never written'); return idx.get(i); });
  return { tensors, ops: lops, input: idx.get(m.inputs[0]), outputs: out, data: Buffer.concat(chunks), halfs };
}

export function writeLnn(g) {
  const head = Buffer.alloc(64);
  head.write('LNN1', 0, 'latin1');
  head.writeUInt32LE(1, 4);
  head.writeUInt32LE(g.tensors.length, 8);
  head.writeUInt32LE(g.ops.length, 12);
  head.writeUInt32LE(g.halfs, 16);
  head.writeUInt32LE(g.input, 20);
  head.writeUInt32LE(g.outputs.length, 24);
  g.outputs.forEach((o, k) => head.writeUInt32LE(o, 28 + 4 * k));
  const tb = Buffer.alloc(32 * g.tensors.length);
  g.tensors.forEach((t, k) => {
    tb.writeUInt32LE(t.kind, 32 * k);
    t.dims.forEach((d, j) => tb.writeUInt32LE(d, 32 * k + 4 + 4 * j));
    tb.writeUInt32LE(t.off, 32 * k + 20);
    tb.writeUInt32LE(t.count, 32 * k + 24);
  });
  const ob = Buffer.alloc(64 * g.ops.length);
  g.ops.forEach((o, k) => {
    ob.writeUInt32LE(o.op, 64 * k);
    o.in.forEach((v, j) => ob.writeInt32LE(v, 64 * k + 4 + 4 * j));
    ob.writeInt32LE(o.out, 64 * k + 16);
    o.p.forEach((v, j) => ob.writeInt32LE(v, 64 * k + 20 + 4 * j));
  });
  const data = Buffer.alloc((g.halfs * 2 + 3) & ~3);
  g.data.copy(data);
  return Buffer.concat([head, tb, ob, data]);
}

// ---- ONNX (a dev-machine reference only; see the header) ----
// A protobuf writer: varints and length-delimited fields are all ONNX needs here.
function varint(n) { const b = []; let v = BigInt(n); if (v < 0n) v += 1n << 64n; do { let x = Number(v & 0x7fn); v >>= 7n; if (v) x |= 0x80; b.push(x); } while (v); return Buffer.from(b); }
const fVar = (f, n) => Buffer.concat([varint((f << 3) | 0), varint(n)]);
const fLen = (f, buf) => Buffer.concat([varint((f << 3) | 2), varint(buf.length), buf]);
const fStr = (f, s) => fLen(f, Buffer.from(s, 'utf8'));
const fF32 = (f, x) => { const b = Buffer.alloc(5); b[0] = (f << 3) | 5; b.writeFloatLE(x, 1); return b; };

function f16to32(h) {
  const s = h >> 15, e = (h >> 10) & 31, f = h & 1023;
  if (e === 0) return (s ? -1 : 1) * f * 2 ** -24;
  if (e === 31) return f ? NaN : (s ? -Infinity : Infinity);
  return (s ? -1 : 1) * (1 + f / 1024) * 2 ** (e - 15);
}

export function toOnnx(g) {
  const nodes = [], inits = [];
  const name = (i) => `t${i}`;
  let tmp = 0;
  const attrI = (n, v) => fLen(5, Buffer.concat([fStr(1, n), fVar(3, v), fVar(20, 2)]));
  const attrF = (n, v) => fLen(5, Buffer.concat([fStr(1, n), fF32(2, v), fVar(20, 1)]));
  const attrS = (n, s) => fLen(5, Buffer.concat([fStr(1, n), fLen(4, Buffer.from(s)), fVar(20, 3)]));
  const attrIs = (n, a) => fLen(5, Buffer.concat([fStr(1, n), ...a.map((v) => fVar(8, v)), fVar(20, 7)]));
  const node = (type, ins, outs, attrs = []) => nodes.push(fLen(1, Buffer.concat([...ins.map((s) => fStr(1, s)), ...outs.map((s) => fStr(2, s)), fStr(4, type), ...attrs])));
  const tensorF32 = (nm, dims, vals) => {
    const raw = Buffer.alloc(vals.length * 4);
    vals.forEach((v, k) => raw.writeFloatLE(v, 4 * k));
    inits.push(fLen(5, Buffer.concat([...dims.map((d) => fVar(1, d)), fVar(2, 1), fStr(8, nm), fLen(9, raw)])));
  };
  const tensorI64 = (nm, vals) => {
    const raw = Buffer.alloc(vals.length * 8);
    vals.forEach((v, k) => raw.writeBigInt64LE(BigInt(v), 8 * k));
    inits.push(fLen(5, Buffer.concat([fVar(1, vals.length), fVar(2, 7), fStr(8, nm), fLen(9, raw)])));
  };
  const constVals = (t) => { const a = []; for (let k = 0; k < t.count; k++) a.push(f16to32(g.data.readUInt16LE(2 * (t.off + k)))); return a; };
  // weights: TFLite OHWI → ONNX OIHW; depthwise 1HWC → C1HW; transposed conv OHWI → IOHW
  const permute = (vals, d, order) => {
    const strides = [d[1] * d[2] * d[3], d[2] * d[3], d[3], 1];
    const nd = order.map((k) => d[k]), out = new Array(vals.length);
    let o = 0;
    for (let a = 0; a < nd[0]; a++) for (let b = 0; b < nd[1]; b++) for (let c = 0; c < nd[2]; c++) for (let e = 0; e < nd[3]; e++) {
      const idx = [0, 0, 0, 0]; idx[order[0]] = a; idx[order[1]] = b; idx[order[2]] = c; idx[order[3]] = e;
      out[o++] = vals[idx[0] * strides[0] + idx[1] * strides[1] + idx[2] * strides[2] + idx[3]];
    }
    return [out, nd];
  };
  const done = new Set();
  const weight = (i, kind) => {
    const nm = `w${i}`;
    if (done.has(nm)) return nm;
    done.add(nm);
    const t = g.tensors[i], v = constVals(t), d = t.dims;
    if (kind === 'bias') tensorF32(nm, [d[3]], v);
    else if (kind === 'conv') { const [o, nd] = permute(v, d, [0, 3, 1, 2]); tensorF32(nm, nd, o); }
    else if (kind === 'dw') { const [o, nd] = permute(v, d, [3, 0, 1, 2]); tensorF32(nm, nd, o); }
    else if (kind === 'tconv') { const [o, nd] = permute(v, d, [3, 0, 1, 2]); tensorF32(nm, nd, o); }
    return nm;
  };
  const act = (a, x, out) => {
    if (a === ACT.NONE) return x;
    node(a === ACT.RELU ? 'Relu' : a === ACT.HSWISH ? 'HardSwish' : 'Sigmoid', [x], [out]);
    return out;
  };
  const autoPad = (p) => attrS('auto_pad', p === PAD_SAME ? 'SAME_UPPER' : 'VALID');
  // activations travel as NCHW; the graph's input and outputs are NHWC like the .lnn
  const inT = g.tensors[g.input].dims;
  node('Transpose', ['input'], [name(g.input)], [attrIs('perm', [0, 3, 1, 2])]);
  for (const o of g.ops) {
    const [x, w, b] = o.in.map((i) => (i >= 0 ? name(i) : ''));
    const y = name(o.out), pre = `${y}_pre`;
    const outT = g.tensors[o.out].dims;
    switch (o.op) {
    case LOP.CONV: case LOP.DWCONV: {
      const wt = g.tensors[o.in[1]].dims;
      const attrs = [autoPad(o.p[2]), attrIs('strides', [o.p[0], o.p[1]]), attrIs('kernel_shape', [wt[1], wt[2]])];
      if (o.op === LOP.DWCONV) attrs.push(attrI('group', wt[3]));
      node('Conv', [x, weight(o.in[1], o.op === LOP.DWCONV ? 'dw' : 'conv'), weight(o.in[2], 'bias')], [o.p[3] ? pre : y], attrs);
      act(o.p[3], pre, y);
      break;
    }
    case LOP.TCONV:
      node('ConvTranspose', [x, weight(o.in[1], 'tconv'), weight(o.in[2], 'bias')], [o.p[3] ? pre : y],
        [attrIs('strides', [2, 2]), attrIs('kernel_shape', [2, 2])]);
      act(o.p[3], pre, y);
      break;
    case LOP.ADD: case LOP.MUL:
      node(o.op === LOP.ADD ? 'Add' : 'Mul', [x, w], [o.p[3] ? pre : y]);
      act(o.p[3], pre, y);
      break;
    case LOP.MEAN: node('GlobalAveragePool', [x], [y]); break;
    case LOP.RESIZE: {
      const sz = `sz${o.out}`;
      tensorI64(sz, [1, outT[3], outT[1], outT[2]]);
      node('Resize', [x, '', '', sz], [y], [attrS('mode', 'linear'),
        attrS('coordinate_transformation_mode', o.p[0] ? 'align_corners' : o.p[1] ? 'half_pixel' : 'asymmetric')]);
      break;
    }
    case LOP.PAD: {
      const pd = `pd${o.out}`;
      tensorI64(pd, [0, o.p[0], 0, 0, 0, o.p[1], 0, 0]);
      node('Pad', [x, pd], [y]);
      break;
    }
    case LOP.MAXPOOL:
      node('MaxPool', [x], [y], [autoPad(o.p[2]), attrIs('strides', [o.p[0], o.p[1]]), attrIs('kernel_shape', [o.p[4], o.p[5]])]);
      break;
    case LOP.RESHAPE: {
      // NCHW → NHWC, then flatten in NHWC order, then back to the NCHW view of [1, h, w, c]
      const a = `rs${o.out}a`, s = `rs${o.out}s`, c = `rs${o.out}c`;
      node('Transpose', [x], [a], [attrIs('perm', [0, 2, 3, 1])]);
      tensorI64(s, outT);
      node('Reshape', [a, s], [c]);
      node('Transpose', [c], [y], [attrIs('perm', [0, 3, 1, 2])]);
      break;
    }
    case LOP.CONCAT: {
      // outer axis: concatenating the NHWC memory end to end, done in NHWC with a flat axis
      const a = `cc${o.out}a`, b2 = `cc${o.out}b`, fa = `cc${o.out}fa`, fb = `cc${o.out}fb`, s = `cc${o.out}s`, f = `cc${o.out}f`, c = `cc${o.out}c`, s2 = `cc${o.out}s2`;
      node('Transpose', [x], [a], [attrIs('perm', [0, 2, 3, 1])]);
      node('Transpose', [w], [b2], [attrIs('perm', [0, 2, 3, 1])]);
      tensorI64(s, [1, -1]);
      node('Reshape', [a, s], [fa]);
      node('Reshape', [b2, s], [fb]);
      node('Concat', [fa, fb], [f], [attrI('axis', 1)]);
      tensorI64(s2, outT);
      node('Reshape', [f, s2], [c]);
      node('Transpose', [c], [y], [attrIs('perm', [0, 3, 1, 2])]);
      break;
    }
    case LOP.RELU: act(ACT.RELU, x, y); break;
    case LOP.HSWISH: act(ACT.HSWISH, x, y); break;
    case LOP.SIGMOID: act(ACT.SIGMOID, x, y); break;
    default: fail(`onnx: op ${o.op}`);
    }
  }
  const outs = g.outputs.map((i, k) => { node('Transpose', [name(i)], [`output${k}`], [attrIs('perm', [0, 2, 3, 1])]); return [`output${k}`, g.tensors[i].dims]; });
  const vinfo = (nm, d) => Buffer.concat([fStr(1, nm), fLen(2, fLen(1, Buffer.concat([fVar(1, 1), fLen(2, Buffer.concat(d.map((v) => fLen(1, fVar(1, v)))))])))]);
  const graph = Buffer.concat([...nodes, fStr(2, 'lnn'), ...inits, fLen(11, vinfo('input', inT)), ...outs.map(([nm, d]) => fLen(12, vinfo(nm, d)))]);
  return Buffer.concat([fVar(1, 8), fStr(2, 'tflite2lnn'), fLen(7, graph), fLen(8, Buffer.concat([fStr(1, ''), fVar(2, 17)]))]);
}

if (process.argv[1] && process.argv[1].endsWith('tflite2lnn.mjs')) {
  const [src, dst] = process.argv.slice(2);
  if (!src || !dst) { console.error('usage: node tools/studio/tflite2lnn.mjs <model.tflite> <out.lnn> [--onnx <out.onnx>]'); process.exit(2); }
  const g = convert(fs.readFileSync(src));
  fs.writeFileSync(dst, writeLnn(g));
  const at = process.argv.indexOf('--onnx');
  if (at > 0) fs.writeFileSync(process.argv[at + 1], toOnnx(g));
  const count = {};
  for (const o of g.ops) count[Object.keys(LOP).find((k) => LOP[k] === o.op)] = (count[Object.keys(LOP).find((k) => LOP[k] === o.op)] || 0) + 1;
  console.log(`${dst}: ${g.tensors.length} tensors, ${g.ops.length} ops ${JSON.stringify(count)}, ${g.halfs} weights`);
}
