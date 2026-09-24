#!/bin/bash
# Gate sweep: build sim/build and gate every scene that has a policy corpus under analysis/polbat_*.
# usage: sweep.sh [label] [build-dir]
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 1

LABEL="${1:-sweep}"
BD="${2:-sim/build}"
OUT="runs/sweep_${LABEL}"
mkdir -p "$OUT"

echo "HEAD $(git log -1 --format='%h %s' | cut -c1-90)" | tee "$OUT/log.txt"
rm -f "$BD/hksim.dll"   # so a build that silently fails to relink is caught below, not masked by a stale DLL
[ -f "$BD/build.ninja" ] || cmake -S sim -B "$BD" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DHKSIM_MODULES=core;hero;phys;fsm;obs" > /dev/null 2>&1
cmake --build "$BD" 2>&1 | grep -E 'error|FAILED' | head -5 | tee -a "$OUT/log.txt"
[ -f "$BD/hksim.dll" ] || { echo "BUILD FAILED: no $BD/hksim.dll" | tee -a "$OUT/log.txt"; exit 1; }
ls -la "$BD/hksim.dll" | awk '{print "DLL", $5, $6, $7, $8}' | tee -a "$OUT/log.txt"

# Discover scenes from the corpora on disk instead of a hard-coded list, so a newly recorded boss is
# swept with no edit here.  analysis/polbat_GG_<Scene> is the convention; polbat_hornet / polbat_gruz
# are the two grandfathered names boss_gate.py's own LEGACY_CORPUS_DIRS maps by hand.
scenes=()
for d in analysis/polbat_GG_*/; do
  [ -d "$d" ] && scenes+=("$(basename "$d" | sed 's/^polbat_//')")
done
[ -d analysis/polbat_hornet ] && scenes+=(GG_Hornet_1)
[ -d analysis/polbat_gruz ] && scenes+=(GG_Gruz_Mother)

for s in "${scenes[@]}"; do
  # -u: a gate killed by the timeout still leaves its partial output (a buffered kill left 0-byte files).
  timeout "${SWEEP_TIMEOUT:-3600}" python -u gate/boss_gate.py "$s" --build-dir "$BD" > "$OUT/gate_$s.txt" 2>&1; rc=$?
  [ $rc -eq 124 ] && echo "TIMEOUT after ${SWEEP_TIMEOUT:-3600}s" >> "$OUT/gate_$s.txt"
  echo "GATE $s rc=$rc :: $(grep -E '^BOSS |^TIMEOUT' "$OUT/gate_$s.txt" | tail -1 | sed -E 's/ +/ /g' | cut -c1-170)" | tee -a "$OUT/log.txt"
done
echo "== done $(date)" | tee -a "$OUT/log.txt"; echo "RESULTS: $OUT/log.txt"
