"""sys.path for the trainer's tests: the repo root (hkpy), train/, and this
directory (probe_worker, which the pool's spawned workers import)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for p in (ROOT, HERE, os.path.join(ROOT, "train")):     # train/ ends up first
    if p not in sys.path:
        sys.path.insert(0, p)
