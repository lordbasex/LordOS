#!/usr/bin/env python3
"""Make LordOS Studio's pictures from the AI-generated originals (approved by the maintainer on 2026-11-22).

Every picture of the catalog (72) and one still per loop the catalog had then (49, new items `still-<loop id without
"loop-">`, in the loop's category and with its name; they stay pictures of their own after the loops moved to 4K) was
generated with ChatGPT for LordOS from the prompts in prompts.json. OpenAI's
terms assign the output to the user; LordOS releases them under CC0 1.0. The loops (kind video) are not touched here.

Inputs (kept outside the repository; untrusted data, so run with `python3 -I`):
  <src>/<id>.png        the originals, 1672x941 (one is 1672x940; kept as it is, never padded or upscaled)
  <src>/prompts.json    [{id, kind, category, name, prompt}]
  <src>/results.json    {id: {sha256, url, date, ...}}: each PNG's SHA-256 is checked before use

Outputs:
  <stage>/studio/pictures/<id>.jpg     JPEG quality 92, 4:4:4, the original's size (published to LordOS-wallpapers)
  <stage>/studio/MANIFEST.json, <stage>/studio/README.md   the pictures' own; tools/studio/publish-studio.py writes the
                                       published folder's (pictures and loops)
  assets/studio/backgrounds/<id>.thumb.jpg   640x360, quality 85
  assets/studio/backgrounds/<id>.jpg         the built-in pictures (the same file as the published one)
  assets/studio/backgrounds.json             the picture items rewritten, the stills added after their category
  <src>/manifest-private.json          each file with its ChatGPT conversation (private: never in the repository)

usage: python3 -I tools/studio/build-ai-backgrounds.py --src DIR --stage DIR
"""
import argparse
import hashlib
import io
import json
import os
import sys

from PIL import Image, ImageOps

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CATALOG = os.path.join(REPO, "assets/studio/backgrounds.json")
THUMBS = os.path.join(REPO, "assets/studio/backgrounds")
JPEG_Q = 92
THUMB_Q = 85
THUMB_W, THUMB_H = 640, 360
AUTHOR = "Generated with ChatGPT for LordOS"
LICENSE = "CC0"
LICENSE_URL = "https://creativecommons.org/publicdomain/zero/1.0/"
SOURCE = "https://github.com/lordbasex/LordOS-wallpapers/tree/main/studio"
DATE = "2026-11-22"
CATEGORIES = ["office", "home", "studio", "nature", "city", "abstract", "space"]


def jpeg(img, q, subsampling):
    out = io.BytesIO()
    img.save(out, "JPEG", quality=q, optimize=True, progressive=False, subsampling=subsampling)
    return out.getvalue()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def still_id(loop_id):
    return "still-" + (loop_id[5:] if loop_id.startswith("loop-") else loop_id)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--stage", required=True)
    a = ap.parse_args()
    prompts = json.load(open(os.path.join(a.src, "prompts.json")))
    results = json.load(open(os.path.join(a.src, "results.json")))
    catalog = json.load(open(CATALOG))
    by_id = {it["id"]: it for it in catalog["items"]}

    made = {}       # catalog id -> (prompt entry, variant)
    private = []
    published = []
    for p in prompts:
        src_id = p["id"]
        r = results[src_id]
        raw = open(os.path.join(a.src, src_id + ".png"), "rb").read()
        if r.get("status") != "done" or hashlib.sha256(raw).hexdigest() != r["sha256"]:
            sys.exit(f"{src_id}: the PNG isn't the one results.json recorded")
        img = Image.open(io.BytesIO(raw))
        img.load()
        img = img.convert("RGB")
        cid = still_id(src_id) if p["kind"] == "video" else src_id
        full = jpeg(img, JPEG_Q, "4:4:4")
        rel = f"pictures/{cid}.jpg"
        write(os.path.join(a.stage, "studio", rel), full)
        thumb = ImageOps.fit(img, (THUMB_W, THUMB_H), Image.LANCZOS)
        write(os.path.join(THUMBS, cid + ".thumb.jpg"), jpeg(thumb, THUMB_Q, "4:2:0"))
        sha = hashlib.sha256(full).hexdigest()
        v = {"name": "full", "w": img.width, "h": img.height, "file": rel, "size": len(full), "sha256": sha}
        made[cid] = (p, v)
        published.append({"id": cid, "file": rel, "size": len(full), "sha256": sha})
        private.append({"id": cid, "from": src_id + ".png", "pngSha256": r["sha256"], "conversation": r.get("url"),
                        "date": r.get("date"), "file": rel, "size": len(full), "sha256": sha})

    def entry(cid):
        p, v = made[cid]
        old = by_id.get(cid)
        it = {"id": cid, "kind": "picture", "category": p["category"], "name": p["name"], "author": AUTHOR,
              "license": LICENSE, "licenseUrl": LICENSE_URL, "source": SOURCE, "ai": True, "variants": [v]}
        if old and old.get("builtin"):
            it["builtin"] = True
            write(os.path.join(THUMBS, cid + ".jpg"), open(os.path.join(a.stage, "studio", v["file"]), "rb").read())
        return it

    items = []
    for it in catalog["items"]:
        if it["kind"] == "picture":
            if it["id"] not in made:
                sys.exit(f"{it['id']}: no generated picture")
            items.append(entry(it["id"]))
        else:
            items.append(it)
    # the stills go after the last picture of their category, in the loops' order
    stills = [cid for cid in made if cid.startswith("still-") and cid not in by_id]
    for cat in CATEGORIES:
        mine = [entry(cid) for cid in stills if made[cid][0]["category"] == cat]
        last = max((i for i, it in enumerate(items) if it["kind"] == "picture" and it["category"] == cat), default=None)
        at = last + 1 if last is not None else len(items)
        items[at:at] = mine
    catalog["items"] = items
    with open(CATALOG + ".part", "w") as f:
        json.dump(catalog, f, indent=1, ensure_ascii=False)
        f.write("\n")
    os.replace(CATALOG + ".part", CATALOG)

    published.sort(key=lambda x: x["file"])
    with open(os.path.join(a.stage, "studio", "MANIFEST.json"), "w") as f:
        json.dump({"note": "LordOS Studio's pictures: size and SHA-256 of every file (the catalog is "
                   "assets/studio/backgrounds.json in the LordOS repository)", "files": published}, f, indent=1)
        f.write("\n")
    with open(os.path.join(a.src, "manifest-private.json"), "w") as f:
        json.dump({"note": "PRIVATE: ChatGPT conversations of LordOS Studio's pictures; never publish", "files": private},
                  f, indent=1)
        f.write("\n")

    rows = []
    for cat in CATEGORIES:
        for it in items:
            if it["kind"] == "picture" and it["category"] == cat:
                v = it["variants"][0]
                rows.append(f"| [`{v['file']}`]({v['file']}) | {it['name']} | {cat} | {v['w']}x{v['h']} | {v['size']} |")
    readme = f"""# LordOS Studio pictures

Background pictures for LordOS Studio, LordOS's camera effects (background replacement). LordOS downloads these files
on demand and keeps one only if its size and SHA-256 are the ones in its catalog, `assets/studio/backgrounds.json`,
whose copy ships in the signed system package. `MANIFEST.json` lists the size and SHA-256 of every file.

## Credits and license

- **Author:** generated with ChatGPT (OpenAI) for LordOS, from prompts written by the LordOS project, on {DATE}.
  Every picture is **AI-generated**: no photograph, person, logo or readable text was used or asked for.
- **License:** [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/). OpenAI's terms assign the output
  to the user; the LordOS project dedicates these pictures to the public domain. Use them for anything, with no
  credit required (a mention of LordOS is welcome).
- **Format:** JPEG, quality 92, 4:4:4 chroma, at the size the generator produced (1672x941; one is 1672x940). Nothing is
  upscaled.

{len(rows)} pictures: {sum(1 for c in made if not c.startswith('still-'))} backgrounds and {len(stills)} stills of moving scenes.

| File | Title | Category | Size | Bytes |
|---|---|---|---|---|
""" + "\n".join(rows) + "\n"
    with open(os.path.join(a.stage, "studio", "README.md"), "w") as f:
        f.write(readme)
    total = sum(x["size"] for x in published)
    print(f"{len(published)} pictures ({len(stills)} new stills), {total} bytes published")


if __name__ == "__main__":
    main()
