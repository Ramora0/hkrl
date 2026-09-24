"""Are the COMMITTED generated scene files still what their generators produce?

    python gate/tables_fresh.py                 # every scene in the build
    python gate/tables_fresh.py --scene GG_X    # just one (~7 s)

Exit 0 = every file is current.  Exit 1 = at least one is stale, and the report names the file and
the first differing line.  Nothing under `sim/` is ever written: each generator runs in a subprocess
whose file writes are redirected into a temp directory.

## Why this exists

**The build never regenerates these files.**  `sim/CMakeLists.txt` runs only `gen_registries.py`,
which imports nothing but `os`/`re`/`sys` and never invokes `gen_tables.py` or `gen_scene.py`.  So
the compiler consumes the COMMITTED `tables_*.c` / `scene_*.c`: a checked-in generated file is a
SOURCE file, and it goes stale the moment someone edits a generator without regenerating every scene
that generator's rules reach.  Nothing in the tree noticed that until this check.

It is not hypothetical.  A SNAPSHOT_RULES entry with no scene qualifier reaches every scene its
pattern matches; regenerating some of those scenes and not others leaves the untouched ones stale by
exactly the fields that entry added.  A one-field drift like that changes when the boss activates,
which changes the whole fight and every number measured on it.

## Why it strips carriage returns, and why that is the whole point

`git checkout` applies `core.autocrlf` and writes these files with CRLF; the generators write LF.  So
a RAW byte compare calls **every** scene stale, and three agents independently reached that false
conclusion in one night -- twice loudly enough to make other agents re-measure work that was fine.

`git status --porcelain` cannot settle it either: it reports an EOL-only change as ` M`,
indistinguishable from a content change, while `git diff` normalises EOL and reports nothing.  The
two disagree BY DESIGN, and on Windows that is routine rather than exotic.  For a checked-in
generated file, "is this difference content or line endings?" is the entire question, so this check
answers it directly by comparing `\\r`-stripped bytes.

## Why a SUBPROCESS per scene, which is not an implementation detail

`gen_tables.main()` is written for one scene per process and keeps module-level caches (e.g.
completeness.py's registered action types).  Generating many scenes in ONE process is therefore not
guaranteed to produce what `python gen_tables.py <scene>` produces.
The first version of this file did exactly that and reported twelve stale files, of which eleven were
its own contamination.  A check that reports false staleness is worse than no check: it trains people
to ignore it.  One process per scene reproduces the real invocation exactly.

## What it does NOT prove

That a generated file is CORRECT -- only that it matches what the current generator produces from the
current dump.  A generator change that is itself wrong passes this check on every scene.
"""
import argparse
import re
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# Runs in a fresh interpreter, one scene per process.  `open` is wrapped rather than the generators
# being edited, so this check needs no cooperation from them and cannot touch the working tree:
# gen_tables honours a module-global OUT_DIR, but gen_scene hard-codes ROOT for both its dump reads
# and its output writes, so redirecting only the writes is the one thing that separates them.
CHILD = r'''
import builtins, io, os, sys
root, gen, scene, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
sys.path.insert(0, os.path.join(root, "sim", "fsm", "gen"))
sys.path.insert(0, os.path.join(root, "sim", "core"))
_open = builtins.open
_gen = os.path.normcase(os.path.join(root, "sim", "generated")) + os.sep
def open2(file, mode="r", *a, **k):
    if isinstance(file, str) and any(m in mode for m in "wa"):
        full = os.path.normcase(os.path.abspath(file))
        if full.startswith(_gen):
            file = os.path.join(out, full[len(_gen):])
            os.makedirs(os.path.dirname(file), exist_ok=True)
        elif os.path.basename(file).startswith(("hero_dump_init", "hero_fields")):
            file = os.path.join(out, os.path.basename(file))
    return _open(file, mode, *a, **k)
builtins.open = open2
sys.stdout = io.StringIO()
if gen == "tables":
    import gen_tables
    gen_tables.main(scene)
elif gen == "hero":
    sys.path.insert(0, os.path.join(root, "sim", "hero"))
    import gen_fields
    gen_fields.main()
else:
    import gen_scene
    gen_scene.main(scene)
'''


def norm(b):
    """The comparison the whole file is about: EOL-insensitive, content-sensitive."""
    return b.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def first_diff(a, b):
    """Line number and both lines, so a failure names the field rather than just the file."""
    la, lb = norm(a).split(b"\n"), norm(b).split(b"\n")
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else b"<eof>"
        y = lb[i] if i < len(lb) else b"<eof>"
        if x != y:
            return i + 1, x[:150].decode("utf-8", "replace"), y[:150].decode("utf-8", "replace")
    return 0, "", ""


# Unity hands out a fresh instanceID per session, so RE-DUMPING a scene changes every one of them and
# nothing else.  gen_scene puts them in `hk_static_collider.instance_id` and derives the `pts_n<iid>`
# blob symbol names from them, so a re-dump makes scene_*.c differ in hundreds of lines that carry no
# geometry.  After masking these two patterns, a re-dump's diff against the committed file is
# IDENTICAL -- every float, layer, tag, flag and marker matches.
#
# This is CLASSIFICATION, not an exemption: the file is still reported and the exit code is still 1.
# It only tells the reader whether they are looking at a generator change that did not reach every
# scene (act on it) or at dump provenance (board trap #7 -- re-dumping a shipped boss).
_ID_PATTERNS = [(re.compile(r"pts_n?\d+"), "PTS"),          # blob symbol name, derived from the iid
                (re.compile(r'(", )-?\d+(, )'), r"\1IID\2")]  # hk_static_collider.instance_id column


def mask_ids(b):
    s = norm(b).decode("utf-8", "replace")
    for pat, rep in _ID_PATTERNS:
        s = pat.sub(rep, s)
    return s


def classify(a, b):
    """(total differing lines, lines that survive instanceID masking)."""
    la, lb = norm(a).split(b"\n"), norm(b).split(b"\n")
    total = sum(1 for i in range(max(len(la), len(lb)))
                if (la[i] if i < len(la) else None) != (lb[i] if i < len(lb) else None))
    ma, mb = mask_ids(a).split("\n"), mask_ids(b).split("\n")
    survive = sum(1 for i in range(max(len(ma), len(mb)))
                  if (ma[i] if i < len(ma) else None) != (mb[i] if i < len(mb) else None))
    return total, survive


def scenes_in_build():
    """Whatever the build actually compiles, so a newly ported boss is covered with no edit here."""
    d = os.path.join(ROOT, "sim", "generated")
    return [s for s in sorted(os.listdir(d)) if os.path.isfile(os.path.join(d, s, "scene.c"))]


def run(gen, scene, out):
    r = subprocess.run([sys.executable, "-c", CHILD, ROOT, gen, scene, out],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise SystemExit("tables_fresh: %s generator failed for %s:\n%s" % (gen, scene, r.stderr[-1500:]))


def compare(scene, out, gen, fname):
    bad = []
    run(gen, scene, out)
    for name in (fname,):
        committed = os.path.join(ROOT, "sim", "generated", scene, name)
        fresh = os.path.join(out, scene, name)
        name = "%s/%s" % (scene, name)
        if not os.path.exists(fresh):
            raise SystemExit("tables_fresh: %s did not produce %s" % (gen, name))
        if not os.path.exists(committed):
            bad.append((name, 0, "<not committed>", "<generated>", 0, 0))
            continue
        a, b = open(committed, "rb").read(), open(fresh, "rb").read()
        if norm(a) != norm(b):
            bad.append((name,) + first_diff(a, b) + classify(a, b))
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default=None, help="check one scene instead of every scene in the build")
    ap.add_argument("--tables-only", action="store_true", help="skip gen_scene")
    a = ap.parse_args(argv)
    scenes = [a.scene] if a.scene else scenes_in_build()
    stale, t0 = [], time.time()
    for sc in scenes:
        with tempfile.TemporaryDirectory(prefix="tablesfresh_") as out:
            stale += [(sc,) + row for row in compare(sc, out, "tables", "tables.c")]
            if not a.tables_only:
                stale += [(sc,) + row for row in compare(sc, out, "scene", "scene.c")]
    if not a.scene:
        # sim/hero/gen_fields.py: one generator, per-scene overrides for every dumped scene.
        with tempfile.TemporaryDirectory(prefix="tablesfresh_") as out:
            run("hero", "*", out)
            for name in ("hero_fields.h", "hero_dump_init.c"):
                committed = os.path.join(ROOT, "sim", "hero", name)
                fresh = os.path.join(out, name)
                if not os.path.exists(fresh):
                    raise SystemExit("tables_fresh: gen_fields.py did not produce %s" % name)
                ca, fb = open(committed, "rb").read(), open(fresh, "rb").read()
                if norm(ca) != norm(fb):
                    stale.append(("(hero, all scenes)", name) + first_diff(ca, fb) + classify(ca, fb))
    if stale:
        print("GENERATED FILES STALE: %d file(s) do not match their generator (EOL-normalised)" % len(stale))
        for sc, name, line, committed, fresh, total, survive in stale:
            kind = ("GENERATOR DRIFT -- a generator rule changed and this scene was not regenerated"
                    if survive else
                    "dump provenance only -- instanceIDs from a re-dump; 0 lines survive ID masking")
            print("  %-24s %s" % (sc, name))
            print("      %d differing line(s), %d survive instanceID masking -> %s" % (total, survive, kind))
            print("      first difference at line %d" % line)
            print("      committed: %s" % committed)
            print("      generator: %s" % fresh)
        print("  FIX: re-run that generator for those scenes and commit the result.")
        print("  NOTE: `git status` calls a CRLF-only change ' M' too -- use `git diff --numstat`,")
        print("        which normalises EOL, to see which files really changed.")
        return 1
    print("generated files fresh: %d scene(s) match their generators (EOL-normalised, %.0fs)"
          % (len(scenes), time.time() - t0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
