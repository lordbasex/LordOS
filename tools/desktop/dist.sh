#!/bin/sh
# Prepare a local RPM source archive with the frontend built from the lockfile.
set -eu
cd "$(dirname "$0")/../.."
npm ci
LORD_PROFILE=desktop npm run build
python3 - <<'PY'
import json
from pathlib import Path
import tarfile

root = Path.cwd()
version = json.loads((root / 'package.json').read_text())['version']
name = f'lord-de-{version}'
out = root / 'dist' / f'{name}.tar.gz'
out.parent.mkdir(exist_ok=True)
exclude = {'.git', 'node_modules', 'build', 'dist', '__pycache__', 'target'}

def select(info):
    if any(p in exclude for p in Path(info.name).parts[1:]):
        return None
    if info.name.endswith(('.rpm', '.deb', '.iso', '.img', '.lpk', '.pyc')):
        return None
    return info

with tarfile.open(out, 'w:gz') as archive:
    for path in ['server', 'runtime', 'sdk', 'apps', 'themes', 'assets', 'third_party',
                 'tests', 'tools/desktop', 'tools/build-theme.mjs', 'tools/code-manifest.mjs', 'packaging/desktop',
                 'packaging/rpm', 'Makefile', 'package.json', 'package-lock.json',
                 'code-manifest.json', 'build-profile.json', 'LICENSE', 'THIRD_PARTY.md', 'README.md', 'docs/STANDALONE.md']:
        archive.add(root / path, arcname=f'{name}/{path}', filter=select)
print(out)
PY
