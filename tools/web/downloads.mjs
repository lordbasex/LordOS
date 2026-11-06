#!/usr/bin/env node
// Builds web/content/downloads.json: for every download the site offers, the newest GitHub release
// that actually carries that file, with its size and SHA-256. Update-only releases (0.8.8.1-0.8.8.3)
// have no ISO, OVA or Raspberry Pi image, so "the latest release" is not where the ISO is: the site's
// buttons link straight to the file instead of sending people to the releases list (#35).
//
//   node tools/web/downloads.mjs [out.json]      (GITHUB_TOKEN is used when set; not needed)
//
// web/build.mjs runs this on every build and falls back to the committed file when GitHub can't be
// reached; the result is also published as https://lordos.org/downloads.json.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = 'lordbasex/LordOS';
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// kind → file name pattern; the first group is the version written in the file name
export const KINDS = {
  'iso-amd64': /^lordos-live-([0-9][0-9.]*)-amd64\.iso$/,
  'iso-arm64': /^lordos-live-([0-9][0-9.]*)-arm64\.iso$/,
  'ova-amd64': /^lordos-([0-9][0-9.]*)-amd64\.ova$/,
  'rpi5': /^lordos-([0-9][0-9.]*)-rpi5-arm64\.img\.xz$/,
  'deb-amd64': /^lordos_([0-9][0-9.]*)_amd64\.deb$/,
  'deb-arm64': /^lordos_([0-9][0-9.]*)_arm64\.deb$/,
  'hwdata': /^lordos-hwdata_([0-9][0-9.]*)_all\.deb$/,
  'imager-mac': /^lordos_imager_([0-9][0-9.]*)_universal\.dmg$/,
  'imager-win': /^lordos_imager_([0-9][0-9.]*)_x64_setup\.exe$/,
  'imager-linux': /^lordos_imager_([0-9][0-9.]*)_amd64\.deb$/,
};

// Picks, per kind, the file from the most recently published release (drafts never count).
export function pick(releases) {
  const out = {};
  const live = releases.filter((r) => !r.draft && r.published_at)
    .sort((a, b) => Date.parse(b.published_at) - Date.parse(a.published_at));
  for (const [kind, re] of Object.entries(KINDS)) {
    for (const rel of live) {
      const a = (rel.assets || []).find((x) => re.test(x.name) && x.state !== 'starter');
      if (!a) continue;
      const sha = typeof a.digest === 'string' && a.digest.startsWith('sha256:') ? a.digest.slice(7) : null;
      out[kind] = {
        file: a.name, url: a.browser_download_url, size: a.size, sha256: sha,
        version: a.name.match(re)[1], release: rel.tag_name, prerelease: !!rel.prerelease,
        published: rel.published_at, sums: `https://github.com/${REPO}/releases/download/${rel.tag_name}/SHA256SUMS`,
        page: rel.html_url,
      };
      break;
    }
  }
  return out;
}

export async function fetchReleases({ timeoutMs = 15000 } = {}) {
  const headers = { Accept: 'application/vnd.github+json', 'User-Agent': 'lordos-site-build' };
  if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
  const all = [];
  for (let page = 1; page <= 5; page++) {
    const res = await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=100&page=${page}`,
      { headers, signal: AbortSignal.timeout(timeoutMs) });
    if (!res.ok) throw new Error(`GitHub API: HTTP ${res.status}`);
    const batch = await res.json();
    all.push(...batch);
    if (batch.length < 100) break;
  }
  return all;
}

export async function build(outFile = path.join(ROOT, 'web/content/downloads.json')) {
  const downloads = pick(await fetchReleases());
  for (const k of ['iso-amd64', 'rpi5', 'deb-amd64']) if (!downloads[k]) throw new Error(`no release carries ${k}`);
  const doc = { format: 1, repo: REPO, generated: new Date().toISOString(), downloads };
  fs.writeFileSync(outFile, JSON.stringify(doc, null, 2) + '\n');
  return doc;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const doc = await build(process.argv[2]);
  for (const [k, v] of Object.entries(doc.downloads)) console.log(`${k.padEnd(13)} ${v.release.padEnd(14)} ${v.file}`);
}
