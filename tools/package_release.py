"""Packs this checkout's prebuilt files into dist/release/<tag>/ and writes release.json, which setup.ps1 reads.

    python tools/package_release.py --tag v0.1.0 --repo <owner>/<repo>
                                    [--model runs/<run>/<ckpt>.pth ...]

Refuses unless every binary provably belongs to this checkout:
  sim      sim/build/hksim.dll reproduces tests/fingerprint.json
  kernels  train/kernels/prebuilt/ was built from these kernel sources for the pinned torch (hkkern --prebuild)
  mod      oracle/bin/Release/HKOracle.dll carries a clean commit whose oracle/ is this checkout's

Then upload dist/release/<tag>/* to the GitHub release <tag> and commit release.json.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "train"))
from hkpy import provenance  # noqa: E402

MOD_FILES = ("HKOracle.dll", "HKOracle.pdb", "Newtonsoft.Json.dll", "websocket-sharp.dll")


def refuse(why):
    raise SystemExit("package_release: refusing, " + why)


def check_sim():
    dll = os.path.join(ROOT, "sim", "build", "hksim.dll")
    if not os.path.exists(dll):
        refuse("no sim/build/hksim.dll (python tools/check.py builds it)")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "fingerprint.py"), "--compare",
                        os.path.join(ROOT, "tests", "fingerprint.json")],
                       env=dict(os.environ, HKSIM_DLL=dll), capture_output=True, text=True)
    if r.returncode:
        refuse("sim/build/hksim.dll does not reproduce tests/fingerprint.json:\n" + (r.stdout + r.stderr)[-800:])
    print("sim      fingerprint matches")
    return [(dll, "hksim.dll")]


def check_kernels():
    import hkkern
    import torch
    if hkkern._prebuilt("hk_fast") is None or hkkern._prebuilt("hk_prep") is None:
        refuse("train/kernels/prebuilt/ is missing or stale: python train/hkkern.py --prebuild")
    print(f"kernels  prebuilt for torch {torch.__version__}")
    d = os.path.join(ROOT, "train", "kernels", "prebuilt")
    return [(os.path.join(d, f), f) for f in ("hk_fast.pyd", "hk_prep.pyd", "prebuilt.json")], torch.__version__


def check_mod():
    d = os.path.join(ROOT, "oracle", "bin", "Release")
    dll = os.path.join(d, "HKOracle.dll")
    if not os.path.exists(dll):
        refuse("no oracle/bin/Release/HKOracle.dll (dotnet build oracle/HKOracle.csproj -c Release)")
    with open(dll, "rb") as fh:
        m = provenance._COMMIT_RE.search(fh.read())
    commit = m.group(1).decode() if m else ""
    why = provenance.mod_mismatch(commit)
    if why:
        refuse(f"the mod build: {why}")
    print(f"mod      HKOracle {commit[:10]}")
    return [(os.path.join(d, f), f) for f in MOD_FILES if os.path.exists(os.path.join(d, f))]


def pack(out_dir, name, files):
    path = os.path.join(out_dir, name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for src, arc in files:
            z.write(src, arc)
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    size = os.path.getsize(path)
    print(f"packed   {name} ({size / 1e6:.1f} MB)")
    return h.hexdigest().upper(), size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--repo", required=True, help="the GitHub repo holding the release, <owner>/<repo>")
    ap.add_argument("--model", action="append", default=[], help="a checkpoint to ship in models/ (repeatable)")
    a = ap.parse_args()
    if subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                      capture_output=True, text=True).stdout.strip():
        refuse("the checkout has uncommitted changes")

    sim = check_sim()
    kernels, torch_version = check_kernels()
    mod = check_mod()
    assets = [("sim", "hksim-win64.zip", "sim/build", sim),
              ("sim", f"kernels-torch{torch_version.replace('+', '-')}.zip", "train/kernels/prebuilt", kernels),
              ("game", "HKOracle.zip", "dist/HKOracle", mod)]
    if a.model:
        assets.append(("models", "models.zip", "models", [(p, os.path.basename(p)) for p in a.model]))

    out = os.path.join(ROOT, "dist", "release", a.tag)
    os.makedirs(out, exist_ok=True)
    manifest = {"tag": a.tag, "repo": a.repo, "assets": []}
    for group, name, dest, files in assets:
        sha, size = pack(out, name, files)
        manifest["assets"].append({"group": group, "file": name, "dest": dest, "sha256": sha, "bytes": size})
    with open(os.path.join(ROOT, "release.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")
    print(f"done     {out}; commit release.json and upload the files to release {a.tag}")


if __name__ == "__main__":
    main()
