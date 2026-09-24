"""Re-record every corpus of a directory through the oracle (script mode) with the current mod, N instances at a
time, into <out>/<name>.a.hktrace (+ .rngdraws.jsonl + .fsmticks.jsonl from FsmTickRecorder) and a stamped
<out>/<name>.corpus.json (hkpy/provenance.py), then check the re-recording against the original by OBS-stream
equality (step count and first differing step).  Every corpus is re-recorded in the one configuration
(hkpy/sim_config.py game_env), whatever its source was recorded in, so a legacy source's re-recording is a
stamped, current corpus of the same actions.  Two re-recordings of one directory are the game-vs-game control
(gate/control.py).  The stamp carries the source's play.  Each episode's mod log is kept as
<out>/logs/<name>.mod.log.  Refuses a deployed mod that carries no commit or was built from a dirty oracle/.

Usage: rerecord.py <corpus_dir> <out_dir> [n_parallel] [tag_prefix] [--env KEY=VAL ...] [--limit N] [--only a,b]
  --env        extra oracle environment (repeatable), e.g. --env HK_ORACLE_LIFECYCLE=1
  --limit      replay only the first N corpora (sorted by name)
  --only       replay only these corpus names (comma-separated)
  --timeout    seconds before an instance is killed (default 1500; the state recorder, HK_ORACLE_STATE=1, needs more)
  --no-verify  do not read the two traces back to compare their OBS streams.  That comparison parses two
               ~20 MB traces per worker, and with 4 workers it has crashed the process (access violation)
               on a loaded machine; skip it when the caller compares the recordings itself, e.g. with
               obs_codec masking rather than raw bytes.
"""
import os, sys, json, threading, queue, time, subprocess, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)   # the worktree this file lives in
sys.path.insert(0, ROOT); sys.path.insert(0, HERE)
from hkpy import hktrace, provenance, sim_config
import run_oracle          # noqa: E402

def obs_payloads(path):
    t = hktrace.read_trace(path)
    return [r.payload for r in t.records if type(r).__name__ == "Obs"]

def main():
    argv, extra_env, limit, only, verify, timeout = [], [], None, None, True, 1500
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--env": extra_env.append(next(it))
        elif a == "--limit": limit = int(next(it))
        elif a == "--only": only = set(next(it).split(","))
        elif a == "--no-verify": verify = False
        elif a == "--timeout": timeout = float(next(it))
        else: argv.append(a)
    cd, out = os.path.abspath(argv[0]), os.path.abspath(argv[1]); npar = int(argv[2]) if len(argv) > 2 else 6
    why = provenance.mod_mismatch(provenance.mod_identity(run_oracle.HK_ORACLE)[0])
    if why:
        raise SystemExit("rerecord: refusing, %s: rebuild and deploy the mod from a clean checkout first" % why)
    prefix = argv[3] if len(argv) > 3 else "rr"
    os.makedirs(out, exist_ok=True); os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    names = sorted(f[:-len(".corpus.json")] for f in os.listdir(cd) if f.endswith(".corpus.json"))
    if only is not None: names = [n for n in names if n in only]
    if limit is not None: names = names[:limit]
    rp = os.path.join(out, "_rerecord_results.json")   # merge by name: several corpus dirs may share one out dir
    try: prev = {r["name"]: r for r in json.load(open(rp))}
    except Exception: prev = {}
    tags = queue.Queue(); [tags.put(f"{prefix}{i}") for i in range(npar)]
    work = queue.Queue(); [work.put(n) for n in names]
    results = []; lock = threading.Lock()
    def worker():
        while True:
            try: name = work.get_nowait()
            except queue.Empty: return
            tag = tags.get(); t0 = time.time()
            cpath = os.path.join(cd, name + ".corpus.json"); ref = os.path.join(cd, name + ".a.hktrace")
            rec = {"name": name}
            tr0 = os.path.join(out, name + ".a.hktrace")
            if prev.get(name, {}).get("written") and os.path.exists(tr0):   # keep a re-recording whose instance exited 0
                rec.update({"written": True, "skipped": True}); tags.put(tag)
                with lock: results.append(rec)
                continue
            try:
                corpus = json.load(open(cpath, encoding="utf-8"))
                # parsing the whole reference trace is slow: only for a corpus with no seed or no provenance stamp
                need = corpus.get("seed") is None or not corpus.get("provenance")
                header = hktrace.read_trace(ref).header if need and os.path.exists(ref) else {}
                seed = corpus.get("seed") if corpus.get("seed") is not None else header.get("capture", {}).get("seed")
                play, _why = provenance.play(corpus, header)
                if play not in provenance.PLAYS:
                    raise provenance.Refused("the source corpus's play is %s" % play)
                tr = os.path.join(out, name + ".a.hktrace")
                env = ["HK_ORACLE_SCRIPT=" + cpath, "HK_ORACLE_TRACE=" + tr]
                env += ["%s=%s" % kv for kv in sorted(sim_config.game_env().items())]
                if seed is not None: env.append("HK_ORACLE_SEED=%d" % int(seed))
                env += extra_env
                modlog = os.path.join(run_oracle.LOCALLOW, f"HKOracle_oracle_{tag}.log")
                if os.path.exists(modlog): os.remove(modlog)   # the mod appends (HKOracle.cs); keep this run's boot only
                rec["rc"] = run_oracle.run(tag, env, timeout, False, os.path.join(out, "logs"))
                if os.path.exists(modlog): shutil.copyfile(modlog, os.path.join(out, "logs", name + ".mod.log"))
                # a killed instance (TIMEOUT) leaves a partial trace: written means the instance exited 0
                rec["written"] = rec["rc"] == 0 and os.path.exists(tr); rec["ticks"] = os.path.exists(tr[:-len(".hktrace")] + ".fsmticks.jsonl")
                if rec["written"]:
                    stamped = dict(corpus, seed=seed, provenance=provenance.stamp(
                        corpus["level"], corpus.get("frames_per_wait", 2), run_oracle.HK_ORACLE,
                        "tools/rerecord.py", play))
                    with open(os.path.join(out, name + ".corpus.json"), "w", encoding="utf-8") as fh:
                        json.dump(stamped, fh)
                lc = tr[:-len(".hktrace")] + ".lifecycle.gz"
                if os.path.exists(lc): rec["lifecycle_bytes"] = os.path.getsize(lc)
                if verify and rec["written"] and os.path.exists(ref):
                    a, b = obs_payloads(ref), obs_payloads(tr)
                    first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
                    rec.update({"ref_steps": len(a), "new_steps": len(b), "first_diff": first, "identical": first is None and len(a) == len(b)})
            except Exception as e:
                rec["error"] = f"{type(e).__name__}: {e}"
            rec["secs"] = round(time.time() - t0)
            with lock:
                results.append(rec); print(json.dumps(rec), flush=True)
            tags.put(tag)
    ts = [threading.Thread(target=worker) for _ in range(npar)]; [t.start() for t in ts]; [t.join() for t in ts]
    prev.update({r["name"]: r for r in results if not r.get("skipped")})
    json.dump(sorted(prev.values(), key=lambda r: r["name"]), open(rp, "w"), indent=1)
    ok = sum(1 for r in results if r.get("identical")); print(f"DONE {ok}/{len(results)} identical to the reference; ticks for {sum(1 for r in results if r.get('ticks'))}")

if __name__ == "__main__":
    main()
