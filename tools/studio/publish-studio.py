#!/usr/bin/env python3
"""Assemble the studio/ folder published to LordOS-wallpapers from the two builders' outputs.

The pictures come from tools/studio/build-ai-backgrounds.py (<stage>/studio/pictures/<id>.jpg) and the loops from
tools/studio/build-backgrounds.py (<stage>/videos/<id>.{2160,1080}.h264.mp4). Every file the catalog
(assets/studio/backgrounds.json) names is checked against its size and SHA-256 and copied into <out>; files the
catalog doesn't name are removed from <out>/pictures and <out>/videos. Then one README.md (credits and licenses of
both kinds) and one MANIFEST.json (path, size and SHA-256 of every file) are written.

usage: python3 -I tools/studio/publish-studio.py --pictures DIR --videos DIR --out DIR
  --pictures   the folder holding pictures/ (build-ai-backgrounds.py's <stage>/studio)
  --videos     the folder holding videos/ (build-backgrounds.py's <stage>)
  --out        the publish folder (LordOS-wallpapers' studio/); it may be the --pictures folder
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CATEGORIES = ["office", "home", "studio", "nature", "city", "abstract", "space"]
AI_DATE = "2026-11-22"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def loop_described(p):
    # the text build-backgrounds.py's described() writes for each loop (kept here so this needs no numpy or PIL)
    if p["op"] == "encode":
        return f"Procedural render at 3840x2160, {p['seconds']} s loop"
    did = f"Trimmed {p['start']}-{round(p['start'] + p['seconds'], 3)} s, crossfaded into a loop ({p.get('crossfade', 1.0)} s)"
    if p.get("crop"):
        did += ", cropped to 16:9"
    return did + ", scaled, audio removed"


def readme(cat, src):
    pics = [i for i in cat["items"] if i["kind"] == "picture"]
    loops = [i for i in cat["items"] if i["kind"] == "video"]
    rec = {i["id"]: i for i in src["items"]}
    nstill = sum(1 for i in pics if i["id"].startswith("still-"))
    L = ["# LordOS Studio backgrounds", "",
         "Backgrounds for LordOS Studio, LordOS's camera effects (background replacement): pictures in `pictures/` and",
         "loops in `videos/`. LordOS downloads these files on demand and keeps one only if its size and SHA-256 are the",
         "ones in its catalog, `assets/studio/backgrounds.json` in the LordOS repository, whose copy ships in the signed",
         "system package. `MANIFEST.json` lists the path, size and SHA-256 of every file.", "",
         "## Pictures", "",
         f"- **Author:** generated with ChatGPT (OpenAI) for LordOS, from prompts written by the LordOS project, on {AI_DATE}.",
         "  Every picture is **AI-generated**: no photograph, person, logo or readable text was used or asked for.",
         "- **License:** [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/). OpenAI's terms assign the output",
         "  to the user; the LordOS project dedicates these pictures to the public domain. Use them for anything, with no",
         "  credit required (a mention of LordOS is welcome).",
         "- **Format:** JPEG, quality 92, 4:4:4 chroma, at the size the generator produced (1672x941; one is 1672x940). Nothing is",
         "  upscaled. `tools/studio/build-ai-backgrounds.py` makes them.", "",
         f"{len(pics)} pictures: {len(pics) - nstill} backgrounds and {nstill} `still-*` pictures of moving scenes.", "",
         "| File | Title | Category | Size | Bytes |", "|---|---|---|---|---|"]
    for c in CATEGORIES:
        for it in pics:
            if it["category"] == c:
                v = it["variants"][0]
                L.append(f"| [`{v['file']}`]({v['file']}) | {it['name']} | {c} | {v['w']}x{v['h']} | {v['size']} |")
    L += ["", "## Loops", "",
          "Every loop keeps the license of its source. Every loop comes in two sizes, 3840x2160 (`.2160.`) and 1920x1080",
          "(`.1080.`), both made from an original at least 3840x2160; nothing is upscaled. They are H.264 (High profile) MP4",
          "without audio, closed into a seamless loop (the part after the clip's end is crossfaded over its first seconds,",
          "from a start where the two ends look alike: `tools/studio/pick-loop-start.py`). Our own renders (author LordOS)",
          "are MIT licensed and made by `tools/studio/gen-studio-loops.py`. `tools/studio/bg-sources.json` records where each",
          "loop came from and how it was made; `tools/studio/build-backgrounds.py` rebuilds them from the originals.", "",
          f"{len(loops)} loops, {2 * len(loops)} files.", "",
          "| Files | Title | Author | License | Source | Original | What we did |", "|---|---|---|---|---|---|---|"]
    for it in loops:
        r = rec[it["id"]]
        lic = f"[{it['license']}]({it['licenseUrl']})" if it.get("licenseUrl") else it["license"]
        files = ", ".join(f"[`{os.path.basename(v['file'])}`]({v['file']})" for v in it["variants"])
        og = r["origin"].get("original") or {}
        orig = f"{og.get('w')}x{og.get('h')}" if og else ""
        author = it.get("credit") or it["author"]
        L.append(f"| {files} | {it['name']} | {author} | {lic} | {it['source']} | {orig} | {loop_described(r['process'])} |")
    L += ["", "Wikimedia Commons files: license as stated on each file page; CC BY and CC BY-SA files require the attribution",
          "shown in the Author column (the catalog's `credit` field). CC BY-SA files stay under CC BY-SA.", ""]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pictures", required=True)
    ap.add_argument("--videos", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    cat = json.load(open(os.path.join(REPO, "assets/studio/backgrounds.json"), encoding="utf-8"))
    src = json.load(open(os.path.join(REPO, "tools/studio/bg-sources.json"), encoding="utf-8"))
    files = []
    for it in cat["items"]:
        root = a.pictures if it["kind"] == "picture" else a.videos
        for v in it["variants"]:
            f = v["file"]
            if not (f.startswith("pictures/") or f.startswith("videos/")) or ".." in f.split("/"):
                sys.exit(f"{it['id']}: unexpected file {f}")
            s, d = os.path.join(root, f), os.path.join(a.out, f)
            if os.path.getsize(s) != v["size"] or sha256(s) != v["sha256"]:
                sys.exit(f"{s}: size or SHA-256 differs from the catalog")
            if not (os.path.exists(d) and os.path.samefile(s, d)):
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copyfile(s, d + ".part")
                os.replace(d + ".part", d)
            files.append({"id": it["id"], "file": f, "size": v["size"], "sha256": v["sha256"]})
    want = {x["file"] for x in files}
    for sub in ("pictures", "videos"):
        for n in os.listdir(os.path.join(a.out, sub)):
            if f"{sub}/{n}" not in want:
                os.remove(os.path.join(a.out, sub, n))
    for x in files:
        d = os.path.join(a.out, x["file"])
        if os.path.getsize(d) != x["size"] or sha256(d) != x["sha256"]:
            sys.exit(f"{d}: size or SHA-256 differs from the catalog after the copy")
    files.sort(key=lambda x: x["file"])
    with open(os.path.join(a.out, "MANIFEST.json"), "w", encoding="utf-8") as f:
        json.dump({"note": "LordOS Studio's backgrounds: path, size and SHA-256 of every file (the catalog is "
                   "assets/studio/backgrounds.json in the LordOS repository)", "files": files}, f, indent=1,
                  ensure_ascii=False)
        f.write("\n")
    with open(os.path.join(a.out, "README.md"), "w", encoding="utf-8") as f:
        f.write(readme(cat, src))
    total = sum(x["size"] for x in files)
    npic = sum(1 for x in files if x["file"].startswith("pictures/"))
    print(f"{len(files)} files ({npic} pictures, {len(files) - npic} loop files), {total} bytes")


if __name__ == "__main__":
    main()
