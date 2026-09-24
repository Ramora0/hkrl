# Float parity

The sim must produce the game's float results bit for bit. That requires both the right build flags and
C# evaluation semantics.

## Toolchain
C11, gcc (mingw64 WinLibs), cmake + ninja. The DLL is loaded with ctypes and has no binding layer.

## Build contract
`sim/CMakeLists.txt` `HKSIM_FP_FLAGS`, with `-O2`:
```
-msse2 -mfpmath=sse -ffp-contract=off -fno-fast-math -fexcess-precision=standard -frounding-math
-fno-associative-math -fno-reciprocal-math
```
SSE math has no excess precision, and nothing fuses into FMA. `hksim_fp_probe(a, b, c)` (`a*b+c`) must not
fuse. Changing these flags is a behaviour change: re-record `tests/fingerprint.json` and re-run the gate.

## Types and library calls
All game quantities are `float` (Mono `System.Single`). Use `sqrtf` and `fabsf` only. Use no `pow` and no
other libm transcendental unless the cited HK code calls one; in that case, note which libm is used, since
its accuracy is a divergence source.

## C# statements: the Mono evaluation stack
Mono evaluates a C# expression in double precision, following ECMA-335 F-stack semantics. It rounds to
float32 only where the code stores to a float local or field, or passes a float argument. So a ported C#
statement is written as double arithmetic with exactly one `(float)` cast per C# float store or float call
argument. Example: `t = (-ax*dxs + -ay*dys) / denom` needs a double numerator, a double division and one
rounding at the store. A single-operation statement rounds once either way. The observation wire is
reproduced byte-for-byte only under this rule (`analysis/specs/port-obs.md` §4).

It applies to ported C#: HeroController, PlayMaker actions, HitboxObserver and tk2d. Native Unity/Box2D
code (physics, `Random`) stays float32 per operation.

## What "match" means
Integer, enum, bool, string and FSM-state fields match exactly. Float fields and observation payloads are
compared bit-exact. The only exception is the mask in `analysis/specs/obs-wire.md` §5: fields the sim
cannot or should not reproduce, such as wall-clock time and diagnostics. The mask is applied by
`hkpy/obs_codec.py` and `hkpy/obs_parity.py`.
