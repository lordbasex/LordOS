// manifest.mjs <root> <rel>: code-manifest.json of <root> with one more file listed (perf_headless.sh's theme copy,
// one level deeper than tools/code-manifest.mjs looks). Unsigned, as every development manifest.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { codeFiles } from '../../../tools/code-manifest.mjs';
const [root, extra] = process.argv.slice(2);
const version = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8')).version;
const files = [...codeFiles(root), extra].sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b))).map((rel) => {
  const data = fs.readFileSync(path.join(root, rel));
  return `    { "path": ${JSON.stringify(rel)}, "size": ${data.length}, "sha256": "${createHash('sha256').update(data).digest('hex')}" }`;
});
fs.writeFileSync(path.join(root, 'code-manifest.json'),
  `{\n  "format": 1,\n  "kind": "lordos-system-code",\n  "version": ${JSON.stringify(version)},\n  "files": [\n${files.join(',\n')}\n  ]\n}\n`);
