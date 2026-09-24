"""Every wall-clock read a ported fight can reach (docs/frame-order.md "Wall-clock reads").

    python tools/wallclock_sites.py [--out sites.txt] [--managed <Managed dir>]

1. Builds tools/wallclock_scan and runs it: every call site in Assembly-CSharp, -firstpass and PlayMaker of an API
   that reads wall-clock time or counts rendered frames (oracle/Record/WallClockSites.cs), read from the IL with
   Mono.Cecil.  Nothing of the game is loaded.
2. Keeps the sites a fight in the dumped scenes can reach: the method's type is a component in a scene dump
   (hierarchy objects and assets), a PlayMaker action type used by a dumped FSM, or engine code every scene runs
   (the PlayMaker runtime, InControl, GameManager, HeroController, iTween).
3. Lists every dumped FSM action instance whose `realTime` field is true (the FsmFloat waits and tweens that read
   FsmTime.RealtimeSinceStartup instead of game time).

One verdict line per stage; the full lists go to --out.  The runtime counterpart is HK_ORACLE_WALLCLOCK=1
(oracle/Record/WallClockCounter.cs): which of these sites fire in a recorded fight.
"""
import argparse
import gzip
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DUMPS = os.path.join(ROOT, "analysis", "dumps")
FSMS = os.path.join(ROOT, "analysis", "fsm")
MANAGED = os.path.join(os.environ.get("HKRL_GAME") or os.path.join(ROOT, "game"), "oracle_Data", "Managed")
# Engine-side types that run in every fight without being a serialized component of the scene.
ALWAYS = ("HutongGames.PlayMaker.Fsm", "HutongGames.PlayMaker.FsmState", "HutongGames.PlayMaker.FsmTime",
          "HutongGames.PlayMaker.FsmLog", "GameManager", "HeroController", "CameraController", "ObjectPool", "iTween")


def scan(managed, tmp):
    out = os.path.join(tmp, "bin")
    proj = os.path.join(ROOT, "tools", "wallclock_scan", "WallClockScan.csproj")
    r = subprocess.run(["dotnet", "build", proj, "-c", "Release", "-o", out, "-p:HkManaged=" + managed],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("wallclock_scan build FAILED:\n" + r.stdout[-2000:])
    jl = os.path.join(tmp, "sites.jsonl")
    subprocess.run([os.path.join(out, "WallClockScan.exe"), managed, jl], check=True, capture_output=True)
    with open(jl, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def owner_type(site):
    m = site["m"]
    return m[:m.rindex(".")]


def scene_types():
    comps, actions, realtime = set(), set(), []
    for scene in sorted(os.listdir(DUMPS)):
        h = os.path.join(DUMPS, scene, "hierarchy.json.gz")
        if not os.path.exists(h):
            continue
        with gzip.open(h, "rt", encoding="utf-8") as f:
            hier = json.load(f)
        for o in hier.get("objects", []) + hier.get("assets", []):
            for c in o.get("components", []):
                comps.add(c.get("type", ""))
        fp = os.path.join(FSMS, scene + ".json")
        if not os.path.exists(fp):
            continue
        with open(fp, encoding="utf-8") as f:
            fsm = json.load(f)
        for m in fsm.get("fsms", []):
            for st in m.get("states", []):
                for i, a in enumerate(st.get("actions", [])):
                    t = a.get("type", "")
                    actions.add(t)
                    for fld in a.get("fields", []):
                        if fld.get("name") != "realTime":
                            continue
                        v = fld.get("value")
                        if isinstance(v, dict):
                            v = v.get("value") if not v.get("useVariable") else "var:" + str(v.get("name"))
                        if v:
                            realtime.append((scene, m.get("path", ""), m.get("fsmName", ""), st.get("name", ""), i,
                                             t.rsplit(".", 1)[-1], v))
    return comps, actions, realtime


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--managed", default=MANAGED)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        sites = scan(a.managed, tmp)
    print("scan: %d wall-clock call sites in %d methods" % (len(sites), len({s["full"] for s in sites})))
    comps, actions, realtime = scene_types()
    types = comps | actions | set(ALWAYS)
    reach = [s for s in sites if owner_type(s).replace("+", "/") in types or owner_type(s) in types
             or owner_type(s).split("+")[0] in types or owner_type(s).startswith("InControl.")]
    print("reachable: %d sites in %d methods (%d component types, %d action types in the dumps)"
          % (len(reach), len({s["full"] for s in reach}), len(comps), len(actions)))
    print("realTime FSM actions: %d instances in %d scenes" % (len(realtime), len({r[0] for r in realtime})))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write("# reachable wall-clock call sites: class | api | method | k\n")
            for s in sorted(reach, key=lambda s: (s["class"], s["m"], s["k"])):
                f.write("%s | %s | %s | %d\n" % (s["class"], s["api"], s["m"], s["k"]))
            f.write("\n# FSM actions with realTime true: scene | object | fsm | state | index | action | value\n")
            for r in realtime:
                f.write(" | ".join(str(x) for x in r) + "\n")
            f.write("\n# every site: class | api | method | k | assembly\n")
            for s in sorted(sites, key=lambda s: (s["asm"], s["m"], s["k"])):
                f.write("%s | %s | %s | %d | %s\n" % (s["class"], s["api"], s["m"], s["k"], s["asm"]))
        print("wrote " + a.out)


if __name__ == "__main__":
    main()
