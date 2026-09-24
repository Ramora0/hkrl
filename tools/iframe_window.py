"""Where the knight's i-frame window ends, in the game's recorded fights and in the sim's replays of them.

    python tools/iframe_window.py [corpus_dir ...] [--dll path] [--jobs 4]

For every HERO_DAMAGE event of a contact source (hazard type 1) that comes d fixed steps after a landed hit
(the hit took health), the table counts (d, landed).  The window's end is the first d at which a call lands:
HeroController.Invulnerable (HC:3835-3844) clears `invulnerable` in the frame its WaitForSeconds(1.3) comes due,
after the env coroutine, so the knight can be hit again from the 67th fixed step after the hit
(analysis/native_specs/native-playerloop.md §7).  The game's traces and the sim's replays of the same corpora
(hkpy/sim_driver.replay) are counted the same way; the verdict compares the two window ends and also fails
when a trace side has no landing in the window, or a landing before it.
"""
import argparse, collections, glob, os, sys, tempfile
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
LO, HI = 60, 70


def table(trace):
    """Counter of (d, landed) for hazard-type-1 HERO_DAMAGE events d fixed steps after a landed hit."""
    hp, hits, dmg = None, [], []
    for r in trace.records:
        if r.kind == 0x10 and r.ev_name == "HERO_DAMAGE":
            a = r.args
            landed = hp is not None and a["hp_after"] < hp
            dmg.append((r.fixed_count, landed, a["hazard_type"]))
            if landed:
                hits.append(r.fixed_count)
            hp = a["hp_after"]
    out = collections.Counter()
    for h in hits:
        for fc, landed, hz in dmg:
            if hz == 1 and LO <= fc - h <= HI:
                out[(fc - h, landed)] += 1
    return out


def one(job):
    corpus_dir, name, dll = job
    from hkpy import hktrace, sim_driver as sd
    t = hktrace.read_trace(os.path.join(corpus_dir, name + ".a.hktrace"))
    game = table(t)
    lib = sd.load(dll)
    fd, out = tempfile.mkstemp(suffix=".hktrace"); os.close(fd)
    try:
        status, _ = sd.replay(lib, corpus_dir, name, out, trace=t)
        sim = table(hktrace.read_trace(out))
    except Exception as ex:  # a refused (legacy) recording or a trap: reported, not counted
        return name, game, None, repr(ex)
    finally:
        os.remove(out)
    return name, game, sim, status


def window_end(tab):
    landed = sorted(d for (d, ok) in tab if ok)
    return landed[0] if landed else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="*")
    ap.add_argument("--dll", default=os.path.join(ROOT, "sim", "build", "hksim.dll"))
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    dirs = a.dirs or sorted(glob.glob(os.path.join(ROOT, "analysis", "polbat_GG_*")))
    jobs = [(d, os.path.basename(p)[:-len(".corpus.json")], os.path.abspath(a.dll))
            for d in dirs for p in sorted(glob.glob(os.path.join(d, "*.corpus.json")))
            if os.path.exists(p[:-len(".corpus.json")] + ".a.hktrace")]
    game, sim, skipped = collections.Counter(), collections.Counter(), 0
    with Pool(a.jobs) as pool:
        for name, g, s, status in pool.imap_unordered(one, jobs):
            game.update(g)
            if s is None:
                skipped += 1
            else:
                sim.update(s)
    for label, tab in (("game", game), ("sim", sim)):
        row = " ".join("%d:%d/%d" % (d, tab[(d, True)], tab[(d, True)] + tab[(d, False)]) for d in range(LO, HI + 1))
        print("%s (d:landed/calls) %s | window ends at +%s" % (label, row, window_end(tab)))
    ge, se = window_end(game), window_end(sim)
    ok = ge is not None and se is not None and ge == se
    print("iframe_window: %s (%d episodes, %d not replayed; game +%s, sim +%s)" % (
        "PASS" if ok else "FAIL", len(jobs), skipped, ge, se))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
