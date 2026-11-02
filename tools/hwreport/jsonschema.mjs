// jsonschema.mjs — the small subset of JSON Schema (2020-12) that apps/sysinfo/hwreport.schema.json uses,
// with no dependencies (the repository has no JSON Schema validator and the workflow installs nothing).
//
//   validate(schema, value, { maxErrors }) → [{ path, message }]   (empty when the value is valid)
//
// Keywords: $ref (only "#/$defs/<name>"), type (one or a list; "integer" is a whole number), const, enum,
// anyOf, properties, required, additionalProperties (false or a schema), propertyNames (pattern),
// maxProperties, items, minItems, maxItems, minLength, maxLength (in code points), pattern (Unicode regex),
// minimum, maximum. Annotations ($schema, $id, $defs, title, description) are ignored. Any other keyword
// throws, so the schema can't grow a rule this validator would silently skip.
const KNOWN = new Set(['$ref', 'type', 'const', 'enum', 'anyOf', 'properties', 'required', 'additionalProperties', 'propertyNames',
  'maxProperties', 'items', 'minItems', 'maxItems', 'minLength', 'maxLength', 'pattern', 'minimum', 'maximum',
  '$schema', '$id', '$defs', 'title', 'description']);

const regexCache = new Map();
const re = (p) => { if (!regexCache.has(p)) regexCache.set(p, new RegExp(p, 'u')); return regexCache.get(p); };

function typeOf(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'array';
  if (typeof v === 'number') return Number.isFinite(v) ? 'number' : 'nonfinite';
  return typeof v;
}
function isType(v, t) {
  const k = typeOf(v);
  if (t === 'integer') return k === 'number' && Number.isInteger(v);
  return k === t;
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

export function resolve(root, s) {
  let n = 0;
  while (s && typeof s.$ref === 'string') {
    const m = /^#\/\$defs\/([A-Za-z0-9_]+)$/.exec(s.$ref);
    if (!m || !root.$defs || !root.$defs[m[1]]) throw new Error(`unsupported $ref ${s.$ref}`);
    s = root.$defs[m[1]];
    if (++n > 32) throw new Error('$ref loop');
  }
  return s;
}

export function validate(schema, value, { maxErrors = 50 } = {}) {
  const errors = [];
  const add = (path, message) => { if (errors.length < maxErrors) errors.push({ path: path || '/', message }); };

  function check(s, v, path, out) {
    if (s === true || s === undefined) return;
    if (s === false) { out(path, 'not allowed'); return; }
    for (const k of Object.keys(s)) if (!KNOWN.has(k)) throw new Error(`unsupported schema keyword "${k}" at ${path || '/'}`);
    if (s.$ref) { check(resolve(schema, s), v, path, out); return; }
    if (s.anyOf) {
      const ok = s.anyOf.some((sub) => { let bad = 0; check(sub, v, path, () => { bad++; }); return bad === 0; });
      if (!ok) out(path, 'matches none of the allowed forms');
      return;
    }
    if (s.type !== undefined) {
      const ts = Array.isArray(s.type) ? s.type : [s.type];
      if (!ts.some((t) => isType(v, t))) { out(path, `must be ${ts.join(' or ')}`); return; }
    }
    if ('const' in s && !same(s.const, v)) { out(path, `must be ${JSON.stringify(s.const)}`); return; }
    if (s.enum && !s.enum.some((e) => same(e, v))) { out(path, 'is not one of the allowed values'); return; }
    const k = typeOf(v);
    if (k === 'nonfinite') { out(path, 'must be a finite number'); return; }
    if (k === 'string') {
      const len = [...v].length;
      if (s.minLength != null && len < s.minLength) out(path, `shorter than ${s.minLength}`);
      if (s.maxLength != null && len > s.maxLength) out(path, `longer than ${s.maxLength}`);
      if (s.pattern != null && !re(s.pattern).test(v)) out(path, 'has characters or a form that is not allowed');
    }
    if (k === 'number') {
      if (s.minimum != null && v < s.minimum) out(path, `below ${s.minimum}`);
      if (s.maximum != null && v > s.maximum) out(path, `above ${s.maximum}`);
    }
    if (k === 'array') {
      if (s.minItems != null && v.length < s.minItems) out(path, `fewer than ${s.minItems} items`);
      if (s.maxItems != null && v.length > s.maxItems) { out(path, `more than ${s.maxItems} items`); return; }
      if (s.items !== undefined) v.forEach((x, i) => check(s.items, x, `${path}/${i}`, out));
    }
    if (k === 'object') {
      const keys = Object.keys(v);
      if (s.maxProperties != null && keys.length > s.maxProperties) { out(path, `more than ${s.maxProperties} properties`); return; }
      for (const r of s.required || []) if (!Object.prototype.hasOwnProperty.call(v, r)) out(path, `missing "${r}"`);
      for (const key of keys) {
        const p = `${path}/${key.replace(/~/g, '~0').replace(/\//g, '~1')}`;
        if (s.propertyNames && s.propertyNames.pattern && !re(s.propertyNames.pattern).test(key)) { out(p, 'name not allowed'); continue; }
        if (s.properties && Object.prototype.hasOwnProperty.call(s.properties, key)) check(s.properties[key], v[key], p, out);
        else if (s.additionalProperties !== undefined) {
          if (s.additionalProperties === false) out(p, 'is not a field of the report');
          else check(s.additionalProperties, v[key], p, out);
        }
      }
    }
  }
  check(schema, value, '', add);
  return errors;
}
