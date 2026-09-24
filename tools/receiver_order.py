"""Which of a contact's two colliders hears its physics callback first, measured over the fight corpus.

    python tools/receiver_order.py [--examples N] [file-or-dir ...]     (default analysis/lifecycle)

Unity delivers each contact's OnTrigger*/OnCollision*2D to the receivers on one collider's object, then to those
on the other's.  The recorder (oracle/Record/LifecycleRecorder.cs) logs each callback with its receiver and the
other collider (aux), so one contact shows up as two adjacent blocks: the receivers on X's object with other = Y,
then the receivers on Y's object with other = X, in one frame, stage and callback.  A block is taken only when
every receiver in each half sits on that half's collider's own object: Unity also forwards a child collider's
contact to its Rigidbody2D's object, and such forwarded callbacks make two neighbouring contacts look like one.

For every such contact it asks whether X, the collider heard first, has the lower instance id
(Collider2D.GetInstanceID; the GameObjects' ids give the same order in the corpus).  Runtime-created objects have
negative ids that fall with creation and scene-loaded ones positive ids, so the corpus separates "lower id first"
from "later created first": the Knight exists before the boss scene loads and has lower ids than the boss scene's
colliders.  docs/engine-lifecycle.md R5.

Prints one verdict line.  The known-answer test is tests/test_receiver_order.py.
"""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

def contact_blocks(events, go_of):
    """events: [(frame, stage, code, receiver_go, other_collider)] in log order, physics callbacks only;
    go_of: collider -> its GameObject.  Returns (blocks, excluded): blocks = [(frame, stage, code, X, Y)] with X
    the collider whose object heard first; excluded = adjacent block pairs rejected (forwarded receivers, or two
    halves with different callbacks, i.e. two different contacts)."""
    runs = []   # [key, [receiver gos]] with key = (frame, stage, code, other)
    for fr, st, code, rgo, other in events:
        key = (fr, st, code, other)
        if runs and runs[-1][0] == key:
            runs[-1][1].append(rgo)
        else:
            runs.append((key, [rgo]))
    blocks, excluded = [], 0
    for (ka, ra), (kb, rb) in zip(runs, runs[1:]):
        if ka[:2] != kb[:2] or ka[3] < 0 or kb[3] < 0:
            continue
        y, x = ka[3], kb[3]
        if ra[0] != go_of.get(x) or rb[0] != go_of.get(y):
            continue                     # not the two halves of the contact between x and y
        if ka[2] != kb[2] or any(g != go_of.get(x) for g in ra) or any(g != go_of.get(y) for g in rb):
            excluded += 1
            continue
        blocks.append((ka[0], ka[1], ka[2], x, y))
    return blocks, excluded


def episode_events(e):
    """(events, go_of) of a lifecycle_rules.Ep, physics callbacks of every stage."""
    import numpy as np
    import lifecycle_rules as L
    ix = np.nonzero(np.isin(e.code, L.PHYS))[0]
    ev = [(int(e.frame[k]), e.ph(k), int(e.code[k]), int(e.go_uid[int(e.w1[k])]), int(e.aux[k])) for k in ix.tolist()]
    return ev, {i: int(e.go_uid[i]) for i in range(e.ninst)}


def measure(paths, examples=0):
    import lifecycle_rules as L
    out = collections.Counter()
    ex = []
    for f in L.find_files(paths):
        e = L.Ep(f)
        ev, go_of = episode_events(e)
        blocks, excl = contact_blocks(ev, go_of)
        out["excluded"] += excl
        for fr, st, code, x, y in blocks:
            low = int(e.uid[x]) < int(e.uid[y])
            out["lower_first" if low else "higher_first"] += 1
            out["go_agrees"] += int((int(e.go_uid[x]) < int(e.go_uid[y])) == low)
            if not low and len(ex) < examples:
                ex.append("%s f%d %s %s: %s before %s" % (e.name, fr, st, L.CN[code], e.path_[x], e.path_[y]))
    return out, ex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*", default=[os.path.join(os.path.dirname(HERE), "analysis", "lifecycle")])
    ap.add_argument("--examples", type=int, default=0)
    a = ap.parse_args()
    c, ex = measure(a.paths, a.examples)
    n = c["lower_first"] + c["higher_first"]
    print("receiver order: %s (lower instance id first in %d of %d contacts; GameObject ids agree in %d; "
          "%d adjacent block pairs excluded)" % ("OK" if n and c["higher_first"] * 100 < n else "FAIL",
                                                  c["lower_first"], n, c["go_agrees"], c["excluded"]))
    for x in ex:
        print("  " + x)


if __name__ == "__main__":
    main()
