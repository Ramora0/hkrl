"""Build the sim at a git revision into sim/build-ref/<rev>/hksim.dll: the reference build that
tools/sim_equal.py and tools/sim_bench.py compare the working tree against (docs/sim-speed.md).

The source comes from `git archive <rev>` (no checkout, no worktree), so any revision can be built next
to the working tree.

    python tools/build_ref.py                # HEAD: the committed sim, for the working tree's changes
    python tools/build_ref.py 42019af        # a given revision
Prints the DLL path.
"""
import argparse, io, os, subprocess, sys, tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def build(rev, jobs):
    rev = git("rev-parse", "--short=10", rev)
    out = os.path.join(ROOT, "sim", "build-ref", rev)
    dll = os.path.join(out, "hksim.dll")
    if os.path.exists(dll):
        return dll
    src = os.path.join(out, "src")
    tar = subprocess.run(["git", "-C", ROOT, "archive", "--format=tar", rev, "sim", "gate"],
                         check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(src, filter="data")
    bld = os.path.join(out, "build")
    # analysis/ dumps are not needed to compile; the table-provenance check is main's job, not the reference's
    env = dict(os.environ, HKSIM_SKIP_INPUTS_CHECK="1")
    subprocess.run(["cmake", "-S", os.path.join(src, "sim"), "-B", bld, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    "-DHKSIM_MODULES=core;hero;phys;fsm;obs", "-DPython3_EXECUTABLE=" + sys.executable],
                   check=True, env=env, capture_output=True)
    r = subprocess.run(["cmake", "--build", bld, "-j", str(jobs)], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build_ref: build of %s failed\n%s" % (rev, (r.stdout + r.stderr)[-2000:]))
    os.replace(os.path.join(bld, "hksim.dll"), dll)
    return dll


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rev", nargs="?", default="HEAD")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    a = ap.parse_args()
    print(build(a.rev, a.jobs))


if __name__ == "__main__":
    main()
