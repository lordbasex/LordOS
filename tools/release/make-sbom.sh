#!/bin/sh
# Writes an SPDX 2.3 JSON software bill of materials (SBOM) for a built LordOS package: the
# lordos .deb itself, what is compiled or bundled into it (third_party/, fonts, data, the React
# packages in the theme bundle) and the Debian packages it depends on, at the versions installed
# on the machine that runs this script (the build VM). See docs/COMPLIANCE.md.
#
#   tools/release/make-sbom.sh <lordos_VERSION_ARCH.deb> [out.spdx.json]
#
# Run it on the Debian 13 build VM, after building the package and with its dependencies
# installed, so the versions resolved are the ones the release was built against. Default output:
# lordos_<version>_<arch>.spdx.json next to the .deb. Put it in the release folder before
# tools/release/sign-release.sh, so SHA256SUMS (and its signature) covers it.
#
# Environment: LORDOS_TAG (release tag for the download URL; default v<version>).
set -eu
deb=${1:?usage: make-sbom.sh <lordos_VERSION_ARCH.deb> [out.spdx.json]}
[ -r "$deb" ] || { echo "make-sbom.sh: cannot read $deb" >&2; exit 1; }
for t in dpkg-deb dpkg-query python3; do
    command -v "$t" >/dev/null || { echo "make-sbom.sh: $t is needed (run it on the Debian build VM)" >&2; exit 1; }
done
if command -v sha256sum >/dev/null; then sha() { sha256sum "$1" | cut -d' ' -f1; }; else sha() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
version=$(dpkg-deb -f "$deb" Version)
arch=$(dpkg-deb -f "$deb" Architecture)
name=$(dpkg-deb -f "$deb" Package)
[ "$name" = lordos ] || { echo "make-sbom.sh: $deb is the package '$name', not lordos" >&2; exit 1; }
out=${2:-$(dirname "$deb")/lordos_${version}_${arch}.spdx.json}
tag=${LORDOS_TAG:-v$version}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
dpkg-deb -x "$deb" "$tmp/root"

# Debian dependencies: one line per relation, "kind<TAB>wanted<TAB>name<TAB>version<TAB>arch".
# Alternatives (a | b) resolve to the first one installed; version constraints and :arch are dropped.
resolve() { # $1 = kind (depends|recommends), $2 = the control field
    printf '%s\n' "$2" | tr ',' '\n' | while IFS= read -r rel; do
        rel=$(printf '%s' "$rel" | sed 's/([^)]*)//g; s/\[[^]]*\]//g; s/^[[:space:]]*//; s/[[:space:]]*$//')
        [ -n "$rel" ] || continue
        found=""
        for alt in $(printf '%s' "$rel" | tr '|' ' '); do
            p=${alt%%:*}
            st=$(dpkg-query -W -f '${db:Status-Abbrev}' "$p" 2>/dev/null || true)
            case "$st" in
                ii*) printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$rel" "$p" \
                        "$(dpkg-query -W -f '${Version}' "$p")" "$(dpkg-query -W -f '${Architecture}' "$p")"
                     found=1; break ;;
            esac
        done
        [ -n "$found" ] || printf '%s\t%s\t%s\t\t\n' "$1" "$rel" "$(printf '%s' "$rel" | sed 's/[[:space:]]*|.*//; s/:.*//')"
    done
}
{
    resolve depends "$(dpkg-deb -f "$deb" Depends)"
    resolve recommends "$(dpkg-deb -f "$deb" Recommends)"
} > "$tmp/deps.tsv"

# Bundled files that are in the package: "published-path<TAB>sha256"
( cd "$tmp/root" && find . -type f \( -path '*/assets/fonts/*.ttf' -o -path '*/assets/monitor/*' \) | LC_ALL=C sort ) |
    while IFS= read -r f; do printf '%s\t%s\n' "${f#./}" "$(sha "$tmp/root/$f")"; done > "$tmp/files.tsv"

# Font versions, when fontconfig's fc-scan is there: "file<TAB>version"
: > "$tmp/fontver.tsv"
if command -v fc-scan >/dev/null; then
    for f in "$tmp"/root/usr/lib/lordos/assets/fonts/*.ttf; do
        [ -f "$f" ] || continue
        printf '%s\t%s\n' "$(basename "$f")" "$(fc-scan --format '%{fontversion}\n' "$f" 2>/dev/null | head -n 1)" >> "$tmp/fontver.tsv"
    done
fi

SBOM_ROOT=$root SBOM_DEB_SHA=$(sha "$deb") SBOM_DEB_NAME=$(basename "$deb") SBOM_VERSION=$version \
SBOM_ARCH=$arch SBOM_TAG=$tag SBOM_TMP=$tmp SBOM_OUT=$out python3 -I - <<'PY'
import datetime, hashlib, json, os, re

E = os.environ
root, tmp = E["SBOM_ROOT"], E["SBOM_TMP"]
version, arch, tag = E["SBOM_VERSION"], E["SBOM_ARCH"], E["SBOM_TAG"]
debname = E["SBOM_DEB_NAME"]
REPO = "https://github.com/lordbasex/LordOS"

def read(p, default=""):
    try:
        with open(p, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return default

def tsv(name):
    with open(os.path.join(tmp, name), encoding="utf-8") as f:
        return [l.rstrip("\n").split("\t") for l in f if l.strip()]

def spdxid(s):
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]+", "-", s).strip("-")

def header_version(path, pattern):
    m = re.search(pattern, read(path)[:400])
    return m.group(1) if m else "NOASSERTION"

def npm_version(pkg):
    try:
        with open(os.path.join(root, "package-lock.json"), encoding="utf-8") as f:
            lock = json.load(f)
        return lock["packages"]["node_modules/" + pkg]["version"]
    except (OSError, KeyError, ValueError):
        return "NOASSERTION"

files = {p: s for p, s in tsv("files.tsv")}
def font_revision(v):
    # fontconfig's fontversion is the font's head.fontRevision as 16.16 fixed point
    v = v.strip()
    return f"{int(v) / 65536:.3f}" if v.isdigit() and int(v) else ""
fontver = {r[0]: font_revision(r[1] if len(r) > 1 else "") for r in tsv("fontver.tsv") if r and r[0]}

def pkg(spdx, name, ver, lic, supplier, home="NOASSERTION", purl=None, comment=None, checksums=None, copyright="NOASSERTION"):
    p = {
        "SPDXID": spdx, "name": name, "versionInfo": ver or "NOASSERTION",
        "supplier": supplier, "downloadLocation": home, "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION", "licenseDeclared": lic, "copyrightText": copyright,
    }
    if home != "NOASSERTION":
        p["homepage"] = home
    if purl:
        p["externalRefs"] = [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl", "referenceLocator": purl}]
    if comment:
        p["comment"] = comment
    if checksums:
        p["checksums"] = [{"algorithm": "SHA256", "checksumValue": c} for c in checksums]
    return p

main_id = "SPDXRef-Package-lordos"
packages = [pkg(main_id, "lordos", version, "MIT", "Organization: IPERFEX",
                purl=f"pkg:deb/lordos/lordos@{version}?arch={arch}",
                checksums=[E["SBOM_DEB_SHA"]],
                copyright="Copyright (c) 2026 iPERFEX and the LordOS contributors",
                comment=f"The LordOS package ({debname}); source: {REPO}")]
packages[0]["downloadLocation"] = f"{REPO}/releases/download/{tag}/{debname}"
packages[0]["homepage"] = "https://lordos.org"
packages[0]["primaryPackagePurpose"] = "APPLICATION"

tp = os.path.join(root, "third_party")
bundled = [
    ("QuickJS", read(os.path.join(tp, "quickjs/VERSION"), "NOASSERTION"), "MIT", "https://bellard.org/quickjs/",
     "JavaScript engine of the interface, compiled into lord-server (third_party/quickjs)"),
    ("Monocypher", read(os.path.join(tp, "monocypher/VERSION"), "NOASSERTION"), "CC0-1.0 OR BSD-2-Clause", "https://monocypher.org",
     "Cryptography of the password vault and keyring, compiled into lord-vault (third_party/monocypher)"),
    ("stb_image", header_version(os.path.join(tp, "stb/stb_image.h"), r"v(\d+\.\d+)"), "MIT OR Unlicense", "https://github.com/nothings/stb",
     "Image decoding (third_party/stb)"),
    ("stb_image_write", header_version(os.path.join(tp, "stb/stb_image_write.h"), r"v(\d+\.\d+)"), "MIT OR Unlicense", "https://github.com/nothings/stb",
     "Image encoding (third_party/stb)"),
    ("stb_truetype", header_version(os.path.join(tp, "stb/stb_truetype.h"), r"v(\d+\.\d+)"), "MIT OR Unlicense", "https://github.com/nothings/stb",
     "Font rasterization (third_party/stb)"),
    ("EFF large wordlist", "2016", "CC-BY-3.0-US", "https://www.eff.org/dice",
     "Passphrase generator, embedded in lord-vault (third_party/wordlists)"),
    ("SecLists 10k most common passwords", "NOASSERTION", "MIT", "https://github.com/danielmiessler/SecLists",
     "Weak-password check, embedded in lord-vault (third_party/wordlists)"),
    ("react", npm_version("react"), "MIT", "https://react.dev", "Bundled into the theme JavaScript at build time"),
    ("react-reconciler", npm_version("react-reconciler"), "MIT", "https://github.com/facebook/react",
     "Bundled into the theme JavaScript at build time"),
    ("scheduler", npm_version("scheduler"), "MIT", "https://github.com/facebook/react",
     "Bundled into the theme JavaScript at build time"),
    ("Unicode Character Database and CLDR", header_version(os.path.join(root, "server/src/ui/unidata.h"), r"Unicode (\d+\.\d+\.\d+)"),
     "Unicode-3.0", "https://www.unicode.org", "Generated tables for text, bidi and emoji names"),
]
contains = []
for name, ver, lic, home, comment in bundled:
    sid = spdxid("Bundled-" + name)
    purl = f"pkg:npm/{name}@{ver}" if name in ("react", "react-reconciler", "scheduler") and ver != "NOASSERTION" else None
    packages.append(pkg(sid, name, ver, lic, "NOASSERTION", home=home, purl=purl, comment=comment))
    contains.append(sid)

def files_like(pattern):
    return sorted(s for p, s in files.items() if re.search(pattern, p))

fonts = [("Inter", "Inter-", "https://rsms.me/inter/"),
         ("Plus Jakarta Sans", "PlusJakartaSans-", "https://github.com/tokotype/PlusJakartaSans"),
         ("JetBrains Mono", "JetBrainsMono-", "https://www.jetbrains.com/lp/mono/")]
for name, prefix, home in fonts:
    vers = sorted({v for f, v in fontver.items() if f.startswith(prefix) and v})
    ver = vers[0] if len(vers) == 1 else ("2.7.1" if prefix == "PlusJakartaSans-" else "NOASSERTION")
    sid = spdxid("Font-" + name)
    packages.append(pkg(sid, name, ver, "OFL-1.1", "NOASSERTION", home=home,
                        checksums=files_like(r"/fonts/" + re.escape(prefix)) or None,
                        comment="Font shipped in usr/lib/lordos/assets/fonts (checksums of its .ttf files)"))
    contains.append(sid)
sid = spdxid("Font-Noto-subsets")
packages.append(pkg(sid, "Noto Sans Arabic, Devanagari, Thai and CJK SC (subsets)", "NOASSERTION", "OFL-1.1", "NOASSERTION",
                    home="https://github.com/notofonts",
                    comment="Subsets built into usr/lib/lordos/bin/lord-bench, not separate files (tools/gen-lbfonts.py)"))
contains.append(sid)

data = [("DB-IP Lite Country", r"/monitor/geoip-country", "CC-BY-4.0", "https://db-ip.com",
         "Offline IP-to-country database of Monitor (assets/monitor/geoip-country.mmdb.xz). IP geolocation by DB-IP."),
        ("Natural Earth", r"/monitor/land\.bin", "LicenseRef-Public-Domain", "https://www.naturalearthdata.com",
         "World map of Monitor (assets/monitor/land.bin)")]
for name, pattern, lic, home, comment in data:
    sid = spdxid("Data-" + name)
    packages.append(pkg(sid, name, "NOASSERTION", lic, "NOASSERTION", home=home,
                        checksums=files_like(pattern) or None, comment=comment))
    contains.append(sid)

rels = [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": main_id}]
rels += [{"spdxElementId": main_id, "relationshipType": "CONTAINS", "relatedSpdxElement": s} for s in contains]

seen = set()
missing = []
for kind, wanted, name, ver, darch in (r + [""] * (5 - len(r)) for r in tsv("deps.tsv")):
    sid = spdxid("Debian-" + name)
    if sid not in seen:
        seen.add(sid)
        if ver:
            packages.append(pkg(sid, name, ver, "NOASSERTION", "Organization: Debian",
                                home=f"https://packages.debian.org/trixie/{name}",
                                purl=f"pkg:deb/debian/{name}@{ver}?arch={darch}&distro=debian-13",
                                comment=f"Debian package; relation in lordos {kind.capitalize()}: {wanted}. "
                                        f"License: /usr/share/doc/{name}/copyright"))
        else:
            missing.append(name)
            packages.append(pkg(sid, name, "NOASSERTION", "NOASSERTION", "Organization: Debian",
                                comment=f"Not installed on the build machine; relation in lordos {kind.capitalize()}: {wanted}"))
    if kind == "depends":
        rels.append({"spdxElementId": main_id, "relationshipType": "DEPENDS_ON", "relatedSpdxElement": sid})
    else:
        rels.append({"spdxElementId": sid, "relationshipType": "OPTIONAL_DEPENDENCY_OF", "relatedSpdxElement": main_id})

now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
ns_hash = hashlib.sha256(f"{E['SBOM_DEB_SHA']}{now}".encode()).hexdigest()[:16]
doc = {
    "spdxVersion": "SPDX-2.3",
    "dataLicense": "CC0-1.0",
    "SPDXID": "SPDXRef-DOCUMENT",
    "name": f"lordos-{version}-{arch}",
    "documentNamespace": f"https://lordos.org/spdx/lordos-{version}-{arch}-{ns_hash}",
    "creationInfo": {"created": now, "creators": ["Tool: make-sbom.sh", "Organization: IPERFEX"],
                     "comment": "Debian package versions are the ones installed on the build machine."},
    "documentDescribes": [main_id],
    "packages": packages,
    "relationships": rels,
    "hasExtractedLicensingInfos": [{
        "licenseId": "LicenseRef-Public-Domain", "name": "Public domain",
        "extractedText": "Released into the public domain by its authors (Natural Earth: https://www.naturalearthdata.com/about/terms-of-use/)."}],
}
out = E["SBOM_OUT"]
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, indent=2, ensure_ascii=False)
    f.write("\n")
with open(out, encoding="utf-8") as f:
    json.load(f)
print(f"wrote {out}: {len(packages)} packages, {len(rels)} relationships")
if missing:
    print("warning: not installed here, listed without a version: " + ", ".join(missing))
PY
