// validate.mjs — checks a shared hardware report (0.8.7, docs/BENCHMARK.md "0.8.7 — Community hardware reports").
//
//   node tools/hwreport/validate.mjs <report.json>...   check files; prints the result, exit 1 if any is invalid
//   node tools/hwreport/validate.mjs --github           the workflow (.github/workflows/hardware-report.yml)
//
// --github reads the issue from the event file GitHub writes ($GITHUB_EVENT_PATH; never from the command line
// or the environment, so no issue text passes through a shell), finds the attached report, downloads and checks
// it (lib.mjs), then comments the result on the issue (one comment, updated on every edit) and sets its labels.
// It talks only to api.github.com with $GITHUB_TOKEN (the workflow gives it `issues: write` and nothing else to
// write) and downloads the attachment without that token. Plain node 18+, no dependencies.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { checkReport, commentBody, findAttachments, fetchAttachment, parseLimited, labelsFor, STATUS_LABELS, MARKER, MAX_BYTES } from './lib.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const schema = JSON.parse(fs.readFileSync(path.join(REPO, 'apps/sysinfo/hwreport.schema.json'), 'utf8'));

// The result for one report's text (already downloaded).
export function checkText(text) {
  let rep;
  try { rep = parseLimited(text); } catch (e) { return { status: 'invalid', errors: [{ path: '/', message: `not readable JSON: ${e.message}` }], labels: labelsFor('invalid', null) }; }
  if (!rep || typeof rep !== 'object' || Array.isArray(rep)) return { status: 'invalid', errors: [{ path: '/', message: 'not a report object' }], labels: labelsFor('invalid', null) };
  return checkReport(rep, schema);
}

// ---- GitHub ----

async function api(method, route, body) {
  const repo = process.env.GITHUB_REPOSITORY || '';
  if (!/^[A-Za-z0-9_.-]{1,100}\/[A-Za-z0-9_.-]{1,100}$/.test(repo)) throw new Error('GITHUB_REPOSITORY is not owner/name');
  const r = await fetch(`https://api.github.com/repos/${repo}${route}`, {
    method,
    headers: {
      authorization: `Bearer ${process.env.GITHUB_TOKEN}`, accept: 'application/vnd.github+json',
      'x-github-api-version': '2022-11-28', 'user-agent': 'lordos-hwreport-check', ...(body ? { 'content-type': 'application/json' } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (r.status === 404 && method === 'DELETE') return null;
  if (!r.ok) throw new Error(`${method} ${route}: ${r.status}`);
  return r.status === 204 ? null : r.json();
}

async function github() {
  const ev = JSON.parse(fs.readFileSync(process.env.GITHUB_EVENT_PATH, 'utf8'));
  const issue = ev && ev.issue;
  const n = issue && issue.number;
  if (!Number.isInteger(n) || n < 1) throw new Error('no issue in the event');
  if (issue.pull_request) return;
  const names = (issue.labels || []).map((l) => (l && typeof l.name === 'string' ? l.name : ''));
  if (!names.includes('hardware-report')) return;

  const urls = findAttachments(issue.body, process.env.GITHUB_REPOSITORY);
  let result, fileName = '';
  if (!urls.length) result = { status: 'missing', labels: ['hardware-report', 'report:missing'] };
  else {
    fileName = decodeURIComponent(urls[0].split('/').pop());
    try { result = checkText(await fetchAttachment(urls[0], { maxBytes: MAX_BYTES })); }
    catch (e) { result = { status: 'invalid', error: e.message, labels: labelsFor('invalid', null) }; }
  }
  const body = commentBody(result, { fileName });

  // one comment of ours, updated on every edit
  const comments = await api('GET', `/issues/${n}/comments?per_page=100`);
  const mine = (comments || []).find((c) => c && c.user && c.user.login === 'github-actions[bot]' && typeof c.body === 'string' && c.body.startsWith(MARKER));
  if (mine) await api('PATCH', `/issues/comments/${mine.id}`, { body });
  else await api('POST', `/issues/${n}/comments`, { body });

  // labels: the status labels that no longer apply go, the new ones come
  for (const l of STATUS_LABELS) if (names.includes(l) && !result.labels.includes(l)) await api('DELETE', `/issues/${n}/labels/${encodeURIComponent(l)}`);
  await api('POST', `/issues/${n}/labels`, { labels: result.labels });
  console.log(`issue #${n}: ${result.status} (${result.labels.join(', ')})`);
}

// ---- command line ----
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  if (args[0] === '--github') {
    github().catch((e) => { console.error(`hardware report check failed: ${e.message}`); process.exit(1); });
  } else if (!args.length) {
    console.error('usage: node tools/hwreport/validate.mjs <report.json>... | --github');
    process.exit(2);
  } else {
    let bad = 0;
    for (const f of args) {
      const st = fs.statSync(f);
      const r = st.size > MAX_BYTES ? { status: 'invalid', errors: [{ path: '/', message: 'too big' }], labels: [] } : checkText(fs.readFileSync(f, 'utf8'));
      console.log(`${f}: ${r.status}  [${r.labels.join(', ')}]`);
      for (const e of r.errors || []) console.log(`  error ${e.path}: ${e.message}`);
      for (const p of r.privacy || []) console.log(`  personal ${p.path}: ${p.kind}`);
      for (const x of r.flags || []) console.log(`  check ${x.id}: ${x.detail}`);
      if (r.status === 'invalid') bad++;
    }
    process.exit(bad ? 1 : 0);
  }
}
