"""Fail when a PlayMaker action type is registered more than once.

act_lookup() returns the first registry entry whose type_short matches, so a second registration of the
same type is dead code at best and, if registry order ever changes, a silent behaviour change.  Zero
duplicates are allowed.

Source-level: every `const act_vtable *const act_registry_<name>[] = { ... };` under sim/fsm is read, and
each `&AV_x` entry is resolved to its type string from the vtable definition (or from the SETVAL /
LISTENER macro that generated it).  An entry that cannot be resolved fails too, so a new generating macro
cannot hide a duplicate.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FSM = os.path.join(ROOT, "sim", "fsm")

VTABLE = re.compile(r"\bact_vtable\s+(AV_\w+)\s*=\s*\{\s*\"([^\"]+)\"")
SETVAL = re.compile(r"^SETVAL\(\s*\w+\s*,\s*(\w+)", re.M)
LISTENER = re.compile(r"^LISTENER\(\s*(\w+)", re.M)
REGISTRY = re.compile(r"\bact_registry_(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)


def main():
    seen, bad = {}, []
    for dirpath, _, files in os.walk(FSM):
        for fn in sorted(files):
            if not fn.endswith(".c"):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            src = open(path, encoding="utf-8", errors="replace").read()
            sym2type = dict(VTABLE.findall(src))
            sym2type.update(("AV_" + t, t) for t in SETVAL.findall(src))
            sym2type.update(("AV_ListenFor" + t, "ListenFor" + t) for t in LISTENER.findall(src))
            for _name, body in REGISTRY.findall(src):
                for sym in re.findall(r"&(AV_\w+)", body):
                    t = sym2type.get(sym)
                    if t is None:
                        bad.append("%s: cannot resolve %s to a type string" % (rel, sym))
                    else:
                        seen.setdefault(t, []).append(rel)
    dups = {t: v for t, v in seen.items() if len(v) > 1}
    for t, v in sorted(dups.items()):
        bad.append("%s registered %d times: %s" % (t, len(v), ", ".join(v)))
    if bad:
        print("  dup_actions: FAIL")
        for b in bad:
            print("    " + b)
        return 1
    print("  dup_actions: %d registered action types, no duplicates" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
