"""Dump every scene in a list through the oracle, N instances at a time. usage: dump_all.py scenes.txt out_root tier"""
import subprocess, sys, os, time, threading, queue, json
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'tools', 'run_oracle.py')
scenes = [l.strip() for l in open(sys.argv[1]) if l.strip()]
out_root = os.path.abspath(sys.argv[2]); tier = sys.argv[3] if len(sys.argv) > 3 else '0'
npar = int(sys.argv[4]) if len(sys.argv) > 4 else 6
os.makedirs(out_root, exist_ok=True)
tags = queue.Queue(); [tags.put(f"dm{i}") for i in range(npar)]
work = queue.Queue(); [work.put(s) for s in scenes]
results = []; lock = threading.Lock()
def worker():
    while True:
        try: s = work.get_nowait()
        except queue.Empty: return
        tag = tags.get()
        t0 = time.time()
        cmd = [sys.executable, RUN, '--tag', tag, '--timeout', '420',
               '--env', f'HK_ORACLE_DUMPS={out_root}', '--env', f'HK_ORACLE_LEVEL={s}', '--env', f'HK_ORACLE_TIER={tier}']
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=480, cwd=ROOT)
            rc, tail = r.returncode, (r.stdout + r.stderr)[-300:].replace('\n', ' | ')
        except subprocess.TimeoutExpired:
            rc, tail = 'TIMEOUT', ''
        d = os.path.join(out_root, s)
        files = sorted(os.listdir(d)) if os.path.isdir(d) else []
        rec = {'scene': s, 'rc': rc, 'secs': round(time.time() - t0), 'files': len(files), 'has_fsm': any(f.startswith('fsm') for f in files), 'tail': tail}
        with lock:
            results.append(rec); print(json.dumps(rec)[:400], flush=True)
        tags.put(tag)
ts = [threading.Thread(target=worker) for _ in range(npar)]
[t.start() for t in ts]; [t.join() for t in ts]
json.dump(results, open(os.path.join(out_root, '_dump_all_results.json'), 'w'), indent=1)
ok = sum(1 for r in results if r['has_fsm']); print(f"DONE {ok}/{len(results)} scenes produced an fsm dump")
