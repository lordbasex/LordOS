#!/usr/bin/env node
// mkdeb-voice.mjs — builds the voice packages (docs/VOICE.md, phase 3) from Kyutai's files, for the signed APT
// repository (tools/release/apt-repo.sh). Every package is Architecture: all and installs under
// /usr/share/lordos/voice, where `say` looks (server/src/tts/say.c):
//
//   lordos-voice-<lang>            models/<lang>/standard.gguf                          (audio.cpp's Q8_0 GGUF)
//   lordos-voice-<lang>-hd         models/<lang>/hd/{model.safetensors,tokenizer.model} (Kyutai's 24-layer model)
//   lordos-voice-<lang>-<voice>    models/<lang>/embeddings/<voice>.safetensors         Depends: lordos-voice-<lang>
//   lordos-voice-<lang>-<voice>-hd models/<lang>/hd/embeddings/<voice>.safetensors      Depends: lordos-voice-<lang>-hd
//   lordos-voice-previews          previews/<lang>/<voice>.wav, a recorded sample of each voice (make-previews.sh)
//
//   node tools/voice/mkdeb-voice.mjs <source folder> <output folder> <version>
//
// The source folder is what tools/voice/fetch-voices.sh downloads: <lang>/standard.gguf, <lang>/embeddings/,
// <lang>/hd/{model.safetensors,tokenizer.model,embeddings/}, and previews/<lang>/<voice>.wav. The voices are
// sdk/voicecatalog.js's, nothing else. A missing file is an error: a package is never built half empty. The packages
// are reproducible (SOURCE_DATE_EPOCH from the version's date, files owned by root, sorted), and stored uncompressed:
// the weights don't compress and xz would take minutes for nothing.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { VOICE_LANGS, VOICES, SOURCE_LICENSES, PREVIEWS_PACKAGE, modelPackage, voicePackage, voiceLabel, hasVoice } =
  await import(path.join(here, '../../sdk/voicecatalog.js'));

const [src, out, version] = process.argv.slice(2);
if (!src || !out || !/^\d{4}\.\d{2}\.\d{2}(\.\d{1,2})?$/.test(version || '')) {
  console.error('usage: mkdeb-voice.mjs <source folder> <output folder> <version: YYYY.MM.DD[.N]>');
  process.exit(2);
}
fs.mkdirSync(out, { recursive: true });
const [y, m, d] = version.split('.').map(Number);
const epoch = Math.floor(Date.UTC(y, m - 1, d) / 1000);
const DEST = 'usr/share/lordos/voice';

const LANG_NAMES = { es: 'Spanish', en: 'English', pt: 'Portuguese' };
const SOURCE_NAMES = {
  kyutai: 'Kyutai, shipped with Pocket TTS',
  'alba-mackenna': 'Alba McKenna, https://huggingface.co/kyutai/tts-voices (alba-mackenna)',
  'voice-zero': 'voice-zero, https://huggingface.co/kyutai/tts-voices (voice-zero)',
  vctk: 'the VCTK corpus (University of Edinburgh), https://huggingface.co/kyutai/tts-voices (vctk)',
};

const PROHIBITED = `Kyutai's terms of use for Pocket TTS apply: it must not be used to clone a voice without the person's
 explicit and lawful consent, to impersonate anyone, to present generated speech as a real recording, or for
 fraud, deception, harassment or any unlawful purpose.`;

function copyright(extra) {
  return `Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Pocket TTS
Source: https://huggingface.co/kyutai/pocket-tts
Comment: Packaged by LordOS (https://lordos.org). ${PROHIBITED.replace(/\n /g, ' ')}

Files: *
Copyright: Kyutai
License: CC-BY-4.0
 Pocket TTS by Kyutai, https://huggingface.co/kyutai/pocket-tts. Licensed under the Creative Commons
 Attribution 4.0 International License, https://creativecommons.org/licenses/by/4.0/
${extra || ''}`;
}

function need(file) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile() || !fs.statSync(file).size) throw new Error(`missing ${file}`);
  return file;
}

// one package: files [[source, path under DEST]], its control fields; returns the .deb's path
function build(name, { depends, description, long, files, extraCopyright }) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lordos-voice-'));
  try {
    for (const [from, to] of files) {
      const dst = path.join(root, DEST, to);
      fs.mkdirSync(path.dirname(dst), { recursive: true });
      try { fs.linkSync(need(from), dst); } catch (e) { if (e.code === 'EXDEV') fs.copyFileSync(from, dst); else throw e; }
      fs.chmodSync(dst, 0o644);
    }
    const doc = path.join(root, 'usr/share/doc', name);
    fs.mkdirSync(doc, { recursive: true });
    fs.writeFileSync(path.join(doc, 'copyright'), copyright(extraCopyright));
    const size = Math.ceil(files.reduce((s, [f]) => s + fs.statSync(f).size, 0) / 1024);
    fs.mkdirSync(path.join(root, 'DEBIAN'));
    fs.writeFileSync(path.join(root, 'DEBIAN/control'), [
      `Package: ${name}`, `Version: ${version}`, 'Architecture: all', 'Section: sound', 'Priority: optional',
      'Maintainer: LordOS <lordos@lordos.org>', `Installed-Size: ${size}`, depends ? `Depends: ${depends}` : null,
      'Homepage: https://lordos.org', `Description: ${description}`, ...long.split('\n').map((l) => ` ${l || '.'}`),
    ].filter(Boolean).join('\n') + '\n');
    // every directory 755, every time the version's date
    for (const p of walk(root)) { if (fs.statSync(p).isDirectory()) fs.chmodSync(p, 0o755); fs.utimesSync(p, epoch, epoch); }
    fs.chmodSync(root, 0o755);
    fs.utimesSync(root, epoch, epoch);
    const deb = path.join(out, `${name}_${version}_all.deb`);
    execFileSync('dpkg-deb', ['--root-owner-group', '-Znone', '--build', root, deb], {
      env: { ...process.env, SOURCE_DATE_EPOCH: String(epoch) }, stdio: ['ignore', 'ignore', 'inherit'] });
    return deb;
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
}

function* walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    yield p;
  }
}

const made = [];
for (const L of VOICE_LANGS) {
  const lang = L.id, name = LANG_NAMES[lang];
  made.push(build(modelPackage(lang, false), {
    description: `${name} voice model for reading aloud (standard)`,
    long: `Kyutai's Pocket TTS model for ${name}, as a Q8_0 GGUF (audio.cpp), for LordOS's \`say\` and Read Aloud.\nFast on any processor. Voices come in their own packages (lordos-voice-${lang}-<voice>).`,
    files: [[path.join(src, lang, 'standard.gguf'), `models/${lang}/standard.gguf`]],
  }));
  made.push(build(modelPackage(lang, true), {
    description: `${name} voice model for reading aloud (HD)`,
    long: `Kyutai's 24-layer Pocket TTS model for ${name} (${L.hd}): more natural speech, about real time\non a recent processor. Voices come in their own packages (lordos-voice-${lang}-<voice>-hd).`,
    files: [[path.join(src, lang, 'hd/model.safetensors'), `models/${lang}/hd/model.safetensors`],
      [path.join(src, lang, 'hd/tokenizer.model'), `models/${lang}/hd/tokenizer.model`]],
  }));
  for (const v of VOICES) {
    const extra = v.source === 'kyutai' ? '' : `\nFiles: *\nComment: the voice "${v.id}" was prepared by Kyutai from a recording by ${SOURCE_NAMES[v.source]}\nLicense: ${SOURCE_LICENSES[v.source]}\n`;
    for (const hd of [false, true]) {
      if (!hasVoice(v.id, hd)) continue;
      made.push(build(voicePackage(lang, v.id, hd), {
        depends: modelPackage(lang, hd),
        description: `${voiceLabel(v.id)}: a ${name} ${hd ? 'HD ' : ''}voice for reading aloud`,
        long: `The voice "${v.id}" (${v.gender === 'f' ? 'female' : 'male'}) for Kyutai's ${name} ${hd ? '24-layer ' : ''}Pocket TTS model.`,
        files: [[path.join(src, lang, hd ? 'hd/embeddings' : 'embeddings', `${v.id}.safetensors`),
          `models/${lang}/${hd ? 'hd/' : ''}embeddings/${v.id}.safetensors`]],
        extraCopyright: extra,
      }));
    }
  }
}
made.push(build(PREVIEWS_PACKAGE, {
  description: 'recorded samples of the voices for reading aloud',
  long: 'A short recording of every LordOS voice in Spanish, English and Portuguese, made with the HD models,\nso a voice can be heard in Settings → AI → Voice before it is downloaded.',
  files: VOICE_LANGS.flatMap((L) => VOICES.map((v) => [path.join(src, 'previews', L.id, `${v.id}.wav`), `previews/${L.id}/${v.id}.wav`])),
  extraCopyright: ['alba-mackenna', 'voice-zero', 'vctk'].map((k) => `\nFiles: previews/*\nComment: voices ${VOICES.filter((v) => v.source === k).map((v) => v.id).join(', ')} were prepared by Kyutai from recordings by ${SOURCE_NAMES[k]}\nLicense: ${SOURCE_LICENSES[k]}\n`).join(''),
}));
for (const f of made) console.log(f);
