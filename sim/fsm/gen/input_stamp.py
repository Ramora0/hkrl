"""sim/fsm/gen/input_stamp.py — records every analysis/ file a generator reads, so gate/inputs_fresh.py
can detect a committed generated table whose source dump changed, by re-hashing only those files
(no regen). analysis/ is an untracked junction, so a replaced dump leaves no trace in `git status`;
gate/tables_fresh.py is the slow full-regen check.

Usage (a generator's __main__ block):

    from input_stamp import InputStamp
    sidecar = os.path.join(OUT_DIR, scene, "tables.inputs.json")
    with InputStamp(sidecar):
        rc = main(scene)
    sys.exit(rc)

While the `with` block is active, every call to builtins.open / io.open / gzip.open that (a) succeeds,
(b) is a read (not write/append) and (c) resolves to a path under the repo's analysis/ directory is
recorded.  On a clean exit (no exception) the sidecar is written: a JSON list of
{"path": <repo-root-relative, forward slashes>, "size": <bytes>, "sha256": <hex digest>}, sorted by
path, plus "scene"/"generator" fields.  On an exception nothing is written (no table was produced).
"""
import builtins
import gzip
import hashlib
import io
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ANALYSIS_DIR = os.path.abspath(os.path.join(ROOT, "analysis"))


def _under_analysis(abspath):
    """True if abspath is analysis/ itself or under it (path-only test against the junction's apparent path)."""
    norm = os.path.normcase(abspath)
    base = os.path.normcase(ANALYSIS_DIR)
    return norm == base or norm.startswith(base + os.sep)


def _is_read_mode(mode):
    """True for a mode that only reads (no 'w', 'a', 'x', or '+' which opens for read+write)."""
    if not isinstance(mode, str):
        return True
    return not any(c in mode for c in "wax+")


def _as_path(file):
    """A str path if `file` names one on disk, else None (file objects, fds, in-memory buffers)."""
    if isinstance(file, str):
        return file
    if isinstance(file, bytes):
        return file.decode("utf-8", "surrogateescape")
    if isinstance(file, os.PathLike):
        return os.fspath(file)
    return None


class InputStamp:
    """Context manager: while active, patches builtins.open/io.open/gzip.open to record every file
    read from under analysis/; writes `sidecar_path` on a clean exit."""

    def __init__(self, sidecar_path, scene=None, generator=None):
        self.sidecar_path = sidecar_path
        self.scene = scene
        self.generator = generator
        self._seen = {}          # abs path -> None, insertion-order dedupe
        self._saved = None       # (builtins.open, io.open, gzip.open)

    def _record(self, file):
        p = _as_path(file)
        if p is None:
            return
        try:
            ap = os.path.abspath(p)
        except Exception:
            return
        if _under_analysis(ap):
            self._seen[ap] = None

    def __enter__(self):
        orig_open, orig_io_open, orig_gzip_open = builtins.open, io.open, gzip.open
        self._saved = (orig_open, orig_io_open, orig_gzip_open)

        def patched_open(file, mode="r", *a, **k):
            fh = orig_open(file, mode, *a, **k)
            if _is_read_mode(mode):
                self._record(file)
            return fh

        def patched_io_open(file, mode="r", *a, **k):
            fh = orig_io_open(file, mode, *a, **k)
            if _is_read_mode(mode):
                self._record(file)
            return fh

        def patched_gzip_open(filename, mode="rb", *a, **k):
            fh = orig_gzip_open(filename, mode, *a, **k)
            if _is_read_mode(mode):
                self._record(filename)
            return fh

        builtins.open = patched_open
        io.open = patched_io_open
        gzip.open = patched_gzip_open
        return self

    def __exit__(self, exc_type, exc, tb):
        builtins.open, io.open, gzip.open = self._saved
        if exc_type is None:
            self._write_sidecar()
        return False   # never suppress an exception from the generator

    def _write_sidecar(self):
        orig_open = self._saved[0]
        entries = []
        for ap in self._seen:
            rel = os.path.relpath(ap, ROOT).replace(os.sep, "/")
            h = hashlib.sha256()
            size = 0
            with orig_open(ap, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
                    size += len(chunk)
            entries.append({"path": rel, "size": size, "sha256": h.hexdigest()})
        entries.sort(key=lambda e: e["path"])
        doc = {"generator": self.generator, "scene": self.scene, "inputs": entries}
        out_dir = os.path.dirname(self.sidecar_path)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        with orig_open(self.sidecar_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(doc, fh, indent=2, sort_keys=True)
            fh.write("\n")
