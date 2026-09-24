# HKOracle: the game mod

One Hollow Knight mod (`oracle/`, assembly `HKOracle`) serves three purposes:
the real-game environment for policy eval, the recorder of traces for the simulator,
and the dumper of the scene data that the sim generators read.

| Dir | Contents |
|---|---|
| `Env/` | `TrainingEnv` (reset/step over the binary WebSocket protocol), protocol, the `Hooks` seam, `RegimeTweaks` |
| `Game/` | scene loading, input shim and hold table, hitbox/FSM observers, pause gate |
| `Record/` | `.hktrace`, `.rngdraws.jsonl`, `.fsmticks.jsonl`, `.lifecycle.gz`, `.methods.jsonl.gz`, `.hkstate` (`docs/state-record.md`), corpus-driven `ScriptDriver` |
| `Dump/` | the per-scene dumps under `analysis/dumps*/<scene>/` |
| `Probe/` | the engine conformance probes (`docs/engine-lifecycle.md` §3) |
| `Mode.cs` | reads the environment once and installs what the launch mode needs |

## Build and install

    cd oracle
    dotnet build -c Release                     # -> oracle/bin/Release/HKOracle.dll, never deploys

Game assemblies come from the oracle install's `oracle_Data\Managed` (`$HKRL_GAME`, default `<repo>\game`;
override with `-p:HkManaged=<dir>`). `tools/make_oracle_install.ps1` creates that install from your Hollow
Knight (found through Steam, or `-Game <dir>`): `oracle.exe`, `oracle_Data` with its own `Managed/Mods`
holding only HKOracle, and the isolation from the real game its header describes (no `steam_api64.dll`,
company "hkrl oracle" so saves and settings live apart, copies of the files the game writes). Every
`<name>_Data` launched from it (`oracle_<tag>`; a training run's eval fleet `ev<i>`; standalone evals and recorders `solo<i>`, `watch<i>`, `rec<i>`; `i<i>`) is a junction to `oracle_Data`,
so one deploy covers them all. With no game from that install running, either rerun the script (it copies
`oracle/bin/Release` into the install) or build straight into it:

    dotnet build -c Release -p:ModsPath=<install>\oracle_Data\Managed\Mods

This replaces `Mods/HKOracle/` with `HKOracle.dll`, `.pdb`, `websocket-sharp.dll`, and `Newtonsoft.Json.dll`.

The build stamps the commit it is made from into the DLL's `AssemblyInformationalVersion`
(`1.0.0+<sha>`, `+<sha>-dirty` when `oracle/` has uncommitted changes; `oracle/ModInfo.cs`). Traces,
draw logs and dumps record it (`mod_commit` / `modCommit`), and the recorders refuse to record with a mod
that has no commit or a dirty one (`hkpy/provenance.py`): deploy from a clean checkout.

## Launch modes

Every mode runs in regime R2 (`docs/frame-order.md`): capture dt 0.02, no Rigidbody2D interpolation
(every `set_interpolation` call site IL-rewritten to `None`), `ShakePositionV2` without its frame-rate
limit, hit-stop off, and a frame clock wherever the game times something on the wall clock inside the
episode (`oracle/Env/RegimeClock.cs`: PlayMaker realtime actions, `WaitForSecondsRealtime`,
`AudioSource.isPlaying`, particle random seeds; `docs/frame-order.md` "Wall-clock reads"). Every agent
step is one frozen frame plus `frames_per_wait` live frames: the request pump blocks inside the frozen frame
until the next request arrives (a peer that stops answering pings ends the env). An instance whose regime pins
fail does not start the env. `frames_per_wait` comes from the reset message. The action interface and the
observation have one configuration, the sim's (`docs/sim-api.md` "One configuration"): the hold table, focus
on Cast, armed rows and pool clones are not switches.

- **Eval** (nothing below set). The env connects to `FK_SERVER_URL` and serves reset/step to
  `train/game_eval.py`. No recorder or dumper is installed.
- **Record**. Set `HK_ORACLE_TRACE`. With `HK_ORACLE_SCRIPT`, a corpus drives the env with no server,
  and the process quits when the corpus ends. Tools: `tools/rerecord.py` (script mode, through
  `tools/run_oracle.py`) and `tools/record_corpus.py` (a checkpoint's play, through the eval fleet).
- **Dump**. Set `HK_ORACLE_DUMPS` and `HK_ORACLE_LEVEL` (and not `HK_ORACLE_SCRIPT`). The mod dumps the
  scene at the first SceneReady, then quits. Tool: `tools/dump_all.py`. With `HK_ORACLE_DUMP_NATIVE=1` it also
  writes `native.hkstate`, the engine's native state at that instant (`docs/state-record.md`).
- **Probe**. Set `HK_ORACLE_PROBE`, `HK_ORACLE_PROBE_SPEC` and `HK_ORACLE_PROBE_OUT`. The mod loads
  `HK_ORACLE_LEVEL` (default GG_Hornet_1), deactivates the boss, makes the knight take no damage, runs the
  selected conformance scenarios, writes their logs to `HK_ORACLE_PROBE_OUT` and quits. Tool:
  `tools/conformance.py`.

The mod log is `%USERPROFILE%\AppData\LocalLow\hkrl oracle\Hollow Knight\HKOracle_<exe>.log` (`Team Cherry`
instead of `hkrl oracle` for an install made before the isolation; `hkpy/paths.py` `data_dir` reads which).

## Environment variables

| Variable | Meaning |
|---|---|
| `FK_SERVER_URL` | WebSocket URL of the trainer (default `ws://localhost:8765`) |
| `FK_REALTIME=1` | cap rendering at 1/dt fps so a graphical eval plays at real speed |
| `HK_ORACLE_TIER` | 0/1/2 = Attuned/Ascended/Radiant statue tier to load (default: the scene's first match) |
| `HK_ORACLE_TRACE` | record mode: trace path; side files share its base name |
| `HK_ORACLE_SEED` | record mode: `Random.InitState(seed)` when the boss scene loads |
| `HK_ORACLE_LIFECYCLE=1` | record mode: also write `<base>[.eN].lifecycle.gz` |
| `HK_ORACLE_METHODS=1` | record mode: also write `<base>.methods.jsonl.gz`, single action and component calls with their inputs and outputs (`docs/method-oracle.md`) |
| `HK_ORACLE_RNG_DRAWS=0` | record mode: skip `.rngdraws.jsonl` (inertness checks) |
| `HK_ORACLE_FSMTICKS=0` | record mode: skip `.fsmticks.jsonl` (inertness checks) |
| `HK_ORACLE_STATE=1` | record mode: also write `<base>[.eN].hkstate`, the full state at the end of every frame (`docs/state-record.md`) |
| `HK_ORACLE_WALLCLOCK=1` | record mode: also write `<base>.wallclock.jsonl`, the wall-clock call sites that fire per episode and owner (`docs/frame-order.md` "Wall-clock reads") |
| `HK_ORACLE_SCRIPT` | corpus JSON (`docs/trace-format.md` §Corpus file); drives the env with no server |
| `HK_ORACLE_DUMPS` | dump mode: output root; files go to `<root>/<scene>/` |
| `HK_ORACLE_LEVEL` | dump mode: boss scene to load (required); probe mode: the scene the probes run in |
| `HK_ORACLE_PROBE` | probe mode: the scenarios to run (`all`, a name, a comma list, or a prefix ending in `*`) |
| `HK_ORACLE_PROBE_SPEC` | probe mode: the scenario file (`spec.json` written by `tools/conformance.py`) |
| `HK_ORACLE_PROBE_OUT` | probe mode: the result file |
| `HK_ORACLE_DUMP_NATIVE=1` | dump mode: also write `<root>/<scene>/native.hkstate` |
