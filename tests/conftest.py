"""Shared test setup: locate/build hksim.dll, and let a test module skip itself if analysis/ data
it needs is missing (analysis/ is a junction to a large out-of-repo data store; it exists on the
machines this normally runs on, but a checkout elsewhere may not have it).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

_DEFAULT_DLL = os.path.join(ROOT, "sim", "build", "hksim.dll")


def _ensure_dll():
    dll = os.environ.get("HKSIM_DLL") or _DEFAULT_DLL
    if not os.path.exists(dll):
        build_dir = os.path.dirname(dll) if os.environ.get("HKSIM_DLL") else os.path.join(ROOT, "sim", "build")
        try:
            if not os.path.isdir(build_dir):
                subprocess.run(["cmake", "-S", "sim", "-B", os.path.relpath(build_dir, ROOT), "-G", "Ninja",
                                "-DCMAKE_BUILD_TYPE=Release", "-DHKSIM_MODULES=core;hero;phys;fsm;obs"],
                               cwd=ROOT, check=True, capture_output=True)
            subprocess.run(["cmake", "--build", os.path.relpath(build_dir, ROOT)], cwd=ROOT,
                           check=True, capture_output=True)
        except Exception as e:
            print("conftest: could not build hksim.dll (%s); tests needing it will fail" % e, file=sys.stderr)
    os.environ["HKSIM_DLL"] = dll


_ensure_dll()


def require_paths(*paths):
    """Call at test-module level: pytest.skip(..., allow_module_level=True) if any path is missing."""
    import pytest
    missing = [p for p in paths if not os.path.exists(p)]
    if missing:
        pytest.skip("analysis data missing: %s" % ", ".join(missing), allow_module_level=True)
