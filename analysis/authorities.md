# Authorities: what settles a question

Every behaviour hksim reproduces belongs to one of the authorities below. Derive it from that authority
before anything else. Running a trace, finding the first divergent frame and patching toward it is the last
resort. A question counts as underivable only after its owning authority has been searched and shown not
to contain the answer.

Recordings (`traces/`, `polbat_*/`) are a **checker**. They show *that* something is wrong, not *what* is
wrong. They validate a port and never define it.

| behaviour | authority | on disk |
|---|---|---|
| Hollow Knight game logic (HeroController, HealthManager, DamageHero, Recoil, boss components) | HK decompile | `decomp/Assembly-CSharp/` |
| HK's FsmStateAction subclasses | HK decompile | `decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/` |
| tk2d sprite animator, iTween | HK decompile | `decomp/Assembly-CSharp{,-firstpass}/`, `decomp/Assembly-CSharp/iTween.cs` |
| PlayMaker runtime: FSM lifecycle, state entry/exit, action order, events, global transitions, variables | PlayMaker decompile | `decomp/PlayMaker/` |
| PlayMaker's Unity plumbing: Update/FixedUpdate/LateUpdate proxies, trigger/collision proxies | PlayMaker decompile | `decomp/PlayMaker/PlayMaker{FSM,FixedUpdate,LateUpdate,TriggerEnter2D,...}.cs` |
| Unity managed engine: Transform, SetActive/OnEnable cascade, Time, the Physics2D API, Rigidbody2D/Collider2D properties | UnityCsReference `2020.2.2f1`, the exact engine version of the game binary | `upstream/UnityCsReference/` |
| Unity 2D physics internals: step, contacts, sensors, TOI, solver | Box2D (Unity's 2D engine is a fork) | `upstream/box2d-v2.3.1/`, `upstream/box2d-v2.4.1/` |
| Unity's native player loop: callback stages and order, Start timing, activation order | the lifecycle recordings (no source exists) | `lifecycle/rules.json`, `docs/engine-lifecycle.md` |
| Concrete scene contents: objects, colliders, bodies, layers, Physics2D settings, hero fields | runtime dumps | `dumps*/<scene>/` |
| Concrete FSM contents: states, transitions, actions, parameter values | runtime FSM dumps | `fsm/<scene>.json`, `dumps_all/<scene>/fsm.json` |
| The observation the agent receives | the mod's packer, transcribed | `specs/obs-wire.md`, `sim/obs/obs.c` |

`specs/*.md` is a cited index into these sources, not a second source of truth. A spec claim justified only
by an experiment is a defect: replace it with a citation or an explicit trap.

What remains without source is Unity's native C++ runtime: its changes to Box2D and its player loop. The
managed side pins the API contract, Box2D pins the algorithm, and the lifecycle recordings pin the loop.
Record a claim as underivable only after showing it is absent from all three.

## Re-creating the upstream trees

`upstream/` is regenerable:

    git clone https://github.com/Unity-Technologies/UnityCsReference.git --depth 1 --branch 2020.2.2f1
    git clone https://github.com/erincatto/box2d.git ; git worktree add box2d-v2.3.1 v2.3.1
                                                     ; git worktree add box2d-v2.4.1 v2.4.1

The engine version comes from the target binary: `hollow_knight.exe` FileVersion 2020.2.2.426360.
