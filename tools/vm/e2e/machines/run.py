# run.py — runs the Machines e2e flows (flows.py) in one headless session (mh.py) with a scratch library.
#   python3 -I tools/vm/e2e/machines/run.py [FLOW ...] [--keep] [--out DIR] [--json FILE]
# FLOW: names from flows.FLOWS (default: all of them, in order). --keep leaves the session's folder (MH_KEEP=1); --out
# copies the screenshots there; --json writes the results and the figures (boot time, storage) for the docs.
# Exit 0 passed, 1 a failure, 77 skipped (no KVM, no image, no build): `make -C server e2e-machines` runs it.
import json, os, signal, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
import flows  # noqa: E402


def main():
    a = sys.argv[1:]
    if "--keep" in a:
        os.environ["MH_KEEP"] = "1"
        a.remove("--keep")
    out_json = None
    if "--out" in a:
        i = a.index("--out"); os.environ["MH_OUT"] = os.path.abspath(a[i + 1]); del a[i:i + 2]
    if "--json" in a:
        i = a.index("--json"); out_json = os.path.abspath(a[i + 1]); del a[i:i + 2]
    names = a or flows.FLOWS
    bad = [n for n in names if n not in flows.FLOWS + flows.EXTRA]
    if bad:
        print("unknown flow(s): %s (known: %s)" % (", ".join(bad), ", ".join(flows.FLOWS)))
        return 2
    # stopped from outside (a timeout, TaskStop): the session and its machines still go (Session.stop in the finally)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    r = mh.Results("machines-e2e")
    arch = {"x86_64": "amd64", "aarch64": "arm64"}.get(os.uname().machine, os.uname().machine)
    c = {"arch": arch}
    import images
    img = images.agent_path(arch)
    files = {"Images/debian13.qcow2": img} if os.path.exists(img) else {}
    if "windows" in names:          # the evaluation and the drivers, when images.py --windows fetched them
        import images
        for rel, src in (("Images/win11-eval-en-us.iso", "win11-eval-en-us.iso"), ("Images/virtio-win.iso", images.pinned("virtio-win")["name"])):
            if os.path.exists(os.path.join(mh.IMAGES, src)):
                files[rel] = os.path.join(mh.IMAGES, src)
    s = mh.Session(files=files)
    try:
        s.start()
    except mh.Skip as e:
        print("skipped: %s" % e)
        return mh.SKIP
    t0 = time.time()
    try:
        for n in names:
            mh.log("-- %s" % n)
            try:
                getattr(flows, "flow_" + n)(s, r, c)
            except mh.Skip as e:
                r.skp(n, e)
            except Exception as e:      # noqa: BLE001 — the next flow still runs
                r.bad(n, "%s: %s" % (type(e).__name__, str(e)[:2000]))
                try:
                    s.shot("fail-%s" % n)
                except Exception:       # noqa: BLE001
                    pass
        errs = s.errors()
        r.t("session: no JS error", not errs, "\n".join(errs[:10]))
    finally:
        c["seconds"] = round(time.time() - t0)
        s.stop()
    if out_json:
        json.dump({"results": {"passed": r.p, "failed": r.f, "skipped": r.s}, "figures": c}, open(out_json, "w"), indent=1, default=str)
    return r.end()


if __name__ == "__main__":
    sys.exit(main())
