"""Fast build-time check: does each COMMITTED generated scene table still agree with the dump files
it was generated from?

    python gate/inputs_fresh.py

Exit 0 = every sidecar's recorded inputs still match the files on disk, and every committed table has
a sidecar.  Exit 1 = at least one scene is stale (an input file changed or went missing) or a
committed table has no sidecar at all, printed with the scene, the offending input, and the fix.
With no dump store under analysis/ at all (a checkout without the data), there is nothing to check: exit 0.

## Why this exists, and why it is not tables_fresh.py

`analysis/` is a directory junction that git does NOT track, so a dump under it can be silently
replaced with no trace in `git status` -- and a committed table (sim/generated/<scene>/tables.c,
sim/generated/<scene>/scene.c) built from the old dump then just looks wrong at runtime, with no clue
that the dump moved out from under it.

`gate/tables_fresh.py` already catches this -- by fully REGENERATING every scene in a
subprocess and diffing against the committed file, ~92s across 15 scenes.  That is too slow to run on
every build.  This script is the fast version: `sim/fsm/gen/input_stamp.py` already recorded, at
generation time, the exact size and sha256 of every analysis/ file each generator read (the sidecars
this script reads: sim/generated/<scene>/{tables,scene}.inputs.json).  Checking
"is the recorded hash still the file's hash" needs no regeneration at all -- it catches a REPLACED
DUMP in about the time it takes to hash the recorded inputs once, and near-instantly on a warm cache.
It does NOT catch a GENERATOR RULE change that every scene's dump still round-trips through unchanged
-- only tables_fresh.py's full regen proves that, which is why both checks exist and neither replaces
the other.

## The cache

Re-hashing everything a generator ever read on every `cmake --build` would defeat the purpose (dumps
run to ~600MB combined), so sha256s are cached keyed on (repo-relative path, size, mtime) in a small
JSON file.  Default location: a hksim-specific folder under the OS temp dir, shared across every build
directory on the machine (the dump files a hash is cached for do not depend on which build invoked the
check).  HKSIM_INPUTS_CACHE overrides the cache file path.

## The escape hatch

HKSIM_SKIP_INPUTS_CHECK=1 skips the check entirely (loud warning to stderr, exit 0) -- for the rare
case of intentionally working with a dump mid-replacement.  sim/CMakeLists.txt does not special-case
this itself; it just always runs this script, and this script honours the variable.
"""
import hashlib
import json
import os
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# sim/generated/<scene>/<kind>.c with its <kind>.inputs.json -- one entry per generator family.
GROUPS = ["tables", "scene"]
# The analysis/ folders the generators read (every sidecar's input paths are under one of these).
STORE_DIRS = ["assets", "dumps", "dumps_all", "dumps_t1", "dumps_t2", "dumps_v2", "fsm", "traces"]


def _cache_path():
    d = os.environ.get("HKSIM_INPUTS_CACHE")
    if d and not d.lower().endswith(".json"):
        os.makedirs(d, exist_ok=True)
        return os.path.join(d, "hash_cache.json")
    if d:
        os.makedirs(os.path.dirname(d) or ".", exist_ok=True)
        return d
    d = os.path.join(tempfile.gettempdir(), "hksim_inputs_fresh")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, "hash_cache.json")


def _load_cache(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return {}


def _save_cache(path, cache):
    tmp = "%s.tmp%d" % (path, os.getpid())
    try:
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(cache, fh)
        os.replace(tmp, path)
    except OSError:
        pass   # the cache is a pure speed optimisation; a write failure must never fail the build


def sha256_cached(abspath, cache):
    """sha256 of the file at abspath, skipping the read when `cache` already has a hash recorded for
    this exact (path, size, mtime)."""
    st = os.stat(abspath)
    key = os.path.relpath(abspath, ROOT).replace(os.sep, "/")
    ent = cache.get(key)
    if ent and ent.get("size") == st.st_size and ent.get("mtime") == st.st_mtime:
        return ent["sha256"]
    h = hashlib.sha256()
    with open(abspath, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    cache[key] = {"size": st.st_size, "mtime": st.st_mtime, "sha256": digest}
    return digest


def scenes_with_table(kind):
    """Scenes committed for this generator family: every sim/generated/<scene>/<kind>.c."""
    d = os.path.join(ROOT, "sim", "generated")
    if not os.path.isdir(d):
        return []
    return [s for s in sorted(os.listdir(d)) if os.path.isfile(os.path.join(d, s, kind + ".c"))]


def check_group(kind, cache):
    """One generator family. Returns a list of (scene, reason, detail) failures."""
    failures = []
    d = os.path.join(ROOT, "sim", "generated")
    for scene in scenes_with_table(kind):
        sidecar_path = os.path.join(d, scene, kind + ".inputs.json")
        sidecar_rel = os.path.relpath(sidecar_path, ROOT).replace(os.sep, "/")
        if not os.path.exists(sidecar_path):
            failures.append((scene, "missing sidecar", sidecar_rel))
            continue
        try:
            with open(sidecar_path, encoding="utf-8") as fh:
                sidecar = json.load(fh)
        except Exception as e:
            failures.append((scene, "unreadable sidecar", "%s (%s)" % (sidecar_rel, e)))
            continue
        # An empty "inputs" list is not itself a failure: a scene generated entirely from git-tracked
        # fixtures (e.g. SYNTH_fsm, built from the committed sim/fsm/gen/SYNTH_fsm.json rather than
        # anything under analysis/) has no dump-provenance risk to check at all -- a change to its
        # input would show up in `git status` like any other tracked file.
        for entry in sidecar.get("inputs") or []:
            rel = entry.get("path", "?")
            abspath = os.path.join(ROOT, rel.replace("/", os.sep))
            if not os.path.isfile(abspath):
                failures.append((scene, "input missing", rel))
                continue
            actual_size = os.path.getsize(abspath)
            if actual_size != entry.get("size"):
                failures.append((scene, "input changed", "%s (recorded size %s, now %s)"
                                  % (rel, entry.get("size"), actual_size)))
                continue
            actual_sha = sha256_cached(abspath, cache)
            if actual_sha != entry.get("sha256"):
                failures.append((scene, "input changed", "%s (recorded sha256 %s..., now %s...)"
                                  % (rel, str(entry.get("sha256"))[:12], actual_sha[:12])))
    return failures


def check_single(sidecar_rel_os, cache):
    """A generator with one output for all scenes (sim/hero/gen_fields.py). -> [(reason, detail)]."""
    sidecar_path = os.path.join(ROOT, sidecar_rel_os)
    rel = sidecar_rel_os.replace(os.sep, "/")
    if not os.path.exists(sidecar_path):
        return [("missing sidecar", rel)]
    try:
        with open(sidecar_path, encoding="utf-8") as fh:
            sidecar = json.load(fh)
    except Exception as e:
        return [("unreadable sidecar", "%s (%s)" % (rel, e))]
    out = []
    for entry in sidecar.get("inputs") or []:
        irel = entry.get("path", "?")
        abspath = os.path.join(ROOT, irel.replace("/", os.sep))
        if not os.path.isfile(abspath):
            out.append(("input missing", irel))
        elif os.path.getsize(abspath) != entry.get("size"):
            out.append(("input changed", "%s (recorded size %s, now %s)" % (irel, entry.get("size"), os.path.getsize(abspath))))
        elif sha256_cached(abspath, cache) != entry.get("sha256"):
            out.append(("input changed", "%s (sha256 differs)" % irel))
    return out


def main(argv=None):
    if os.environ.get("HKSIM_SKIP_INPUTS_CHECK") == "1":
        print("inputs_fresh: HKSIM_SKIP_INPUTS_CHECK=1 -- SKIPPING the dump-provenance check. "
              "A committed scene table may no longer match the dump it was generated from!",
              file=sys.stderr)
        return 0
    # A checkout without the data store (anyone but its owner: analysis/ holds only the tracked docs) has no
    # dump to compare against, so the committed tables are used as they are. A store that is present but
    # lacks one input still fails below ("input missing").
    if not any(os.path.isdir(os.path.join(ROOT, "analysis", d)) for d in STORE_DIRS):
        print("inputs_fresh: no dump store under analysis/ -- using the committed tables unchecked")
        return 0
    t0 = time.time()
    cache_path = _cache_path()
    cache = _load_cache(cache_path)
    failures = []
    n_scenes = 0
    for kind in GROUPS:
        n_scenes += len(scenes_with_table(kind))
        failures += [(scene, kind, reason, detail) for scene, reason, detail in check_group(kind, cache)]
    # sim/hero/gen_fields.py is one generator for every scene: one sidecar, checked the same way.
    hero_failures = check_single(os.path.join("sim", "hero", "hero_dump_init.inputs.json"), cache)
    failures += [("(hero init, all scenes)", "hero", reason, detail) for reason, detail in hero_failures]
    _save_cache(cache_path, cache)
    if failures:
        by_scene = {}
        for scene, prefix, reason, detail in failures:
            by_scene.setdefault(scene, []).append((prefix, reason, detail))
        print("INPUTS STALE: %d scene(s) no longer match the sidecars recorded at generation time"
              % len(by_scene))
        for scene in sorted(by_scene):
            print("  %s" % scene)
            for prefix, reason, detail in by_scene[scene]:
                print("      [%s] %s: %s" % (prefix, reason, detail))
            if scene.startswith("(hero"):
                print("      fix: regenerate: python sim/hero/gen_fields.py")
            else:
                print("      fix: regenerate: python sim/fsm/gen/gen_tables.py %s and python sim/core/gen_scene.py %s"
                      % (scene, scene))
        print("  A stale scene means the dump under analysis/ changed (or the sidecar/table was never")
        print("  committed) since the table was generated -- analysis/ is a directory junction git does")
        print("  not track, so this can happen with no trace in `git status`.")
        return 1
    print("inputs fresh: %d scene(s) match their recorded dump provenance (%.2fs)"
          % (n_scenes, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
