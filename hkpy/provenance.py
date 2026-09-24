"""Corpus provenance: what a recorded corpus was recorded against, and whether a gate may measure it.

Every recorder that writes a `<name>.corpus.json` adds `stamp(...)` under its "provenance" key:

    dump_sha256   the scene's dump as the sim's generated tables name it (sim/generated/<level>/
                  tables.inputs.json, the analysis/dumps inputs only); the sim replays from that dump
    mod_commit    the commit the deployed HKOracle.dll was built from (HKOracle.csproj stamps it into
                  AssemblyInformationalVersion; "" when the DLL predates the stamp)
    mod_sha256    the deployed DLL's bytes
    sim_keys      the hksim configuration the recording ran in (hkpy/sim_config.py sim_keys)
    game_env      the HKOracle switches the game ran with (sim_config.game_env)
    play          "policy" (a checkpoint chose every action) or "script" (the actions were generated)
    frames_per_wait, recorder

The sim and the mod have one configuration (hkpy/sim_config.py).  A recording made in anything else is
LEGACY (`legacy_reason`): the sim replays it in the one configuration, and the gate says why the two may
differ.  That is every recording without a stamp, every one whose trace header carries `capture.oracle_env`
(written only by mods that still read the opt-in switches RETIRED_SWITCHES, whether they were set or not),
and every one stamped with other sim_keys or game_env.

`check(corpus_dir)` is what every gate calls before replaying a corpus directory:
    ok       stamped in the one configuration, the dump is the one the sim is generated from, oracle/ at
             mod_commit is the oracle/ of this checkout (the observer the sim ports), or differs from it
             only in paths listed in SAFE_ORACLE_DIFF (each provably cannot affect a recorded episode)
    refuse   another dump, a mod built from an oracle/ that differs outside SAFE_ORACLE_DIFF, a dirty or
             unknown mod build, a trace header whose switches lie outside regime R2, or corpora in one
             directory stamped differently
    legacy   measured, and labelled legacy with the reason
"""
import hashlib
import json
import os
import re
import struct
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD_DLL_REL = os.path.join("oracle_Data", "Managed", "Mods", "HKOracle", "HKOracle.dll")
_COMMIT_RE = re.compile(rb"\+([0-9a-f]{40}(?:-dirty)?)\x00?")
PLAYS = ("policy", "script")
HOLD_ACTIONS = (1, 3, 5, 6)   # nail_charge, focus, dream_nail, super_dash (docs/sim-api.md Actions)
# The opt-in behaviour switches of the mods before the one configuration, now built into the mod and the sim.
RETIRED_SWITCHES = ("HK_ORACLE_FOCUS_ON_CAST", "HK_ORACLE_ARMED_ROWS", "HK_ORACLE_POOL_CLONES",
                    "HK_ORACLE_HOLD_S", "HK_ORACLE_HOLD_DT")
# Those mods' regime switches: regime R2 (all the sim models) is HK_ORACLE_CAPTURE_DT unset or 0.02, and
# interpolation off and ShakePositionV2.FpsLimit 0, which these two undid only when "0".
R2_OFF_IF_ZERO = ("HK_ORACLE_NOINTERP", "HK_ORACLE_SHAKE_FPS0")

# oracle/ paths (relative to oracle/) that may differ between a corpus's stamped mod_commit and this
# checkout's HEAD without refusing the corpus: each is provably confined to code that runs before a
# recorded episode starts, or that cannot change what a recorder captures once it does, so a corpus
# recorded under the old file replays identically under the new one.
SAFE_ORACLE_DIFF = (
    # Game/SceneHooks.cs: the Hall of Gods canonical-load coroutine (LoadBossScene and its helpers). It
    # drives the pre-fight menu/statue FSM (BossChallengeUI, bossUIControlFSM) from GG_Workshop up to
    # the scene transition into the boss arena, then polls for that transition to finish. Every
    # recorder (TraceRecorder, MethodRecorder, StateRecorder) starts sampling only after the boss scene
    # has loaded and the training/record loop takes over -- nothing in this file runs, or can run,
    # while a recorder is capturing frames, so a change confined to it changes only how reliably the
    # loader reaches the arena, never a frame any corpus recorded once there.
    "Game/SceneHooks.cs",
)


def _oracle_diff_files(commit):
    """oracle/-relative paths that differ between `commit` and HEAD (git diff --name-only -- oracle),
    or None if `commit` is not a commit in this repository."""
    try:
        out = subprocess.check_output(
            ["git", "-C", ROOT, "diff", "--name-only", commit, "HEAD", "--", "oracle"],
            text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, OSError):
        return None
    files = [ln.strip() for ln in out.splitlines() if ln.strip()]
    return [f[len("oracle/"):] if f.startswith("oracle/") else f for f in files]


class Refused(Exception):
    """A recording no gate may measure; the message says why."""


def dump_sha256(level):
    """sha256 over the analysis/dumps inputs the sim's tables for `level` were generated from."""
    p = os.path.join(ROOT, "sim", "generated", level, "tables.inputs.json")
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as fh:
        inputs = json.load(fh)["inputs"]
    rows = sorted("%s %s" % (i["path"], i["sha256"]) for i in inputs
                  if i["path"].replace("\\", "/").startswith("analysis/dumps/"))
    return hashlib.sha256("\n".join(rows).encode()).hexdigest() if rows else None


def mod_identity(install_dir):
    """(mod_commit, mod_sha256) of the HKOracle.dll deployed in an oracle install."""
    p = os.path.join(install_dir, MOD_DLL_REL)
    with open(p, "rb") as fh:
        b = fh.read()
    m = _COMMIT_RE.search(b)
    return (m.group(1).decode() if m else ""), hashlib.sha256(b).hexdigest()


def stamp(level, frames_per_wait, install_dir, recorder, play):
    """The provenance block a recorder writes into each corpus it records."""
    from hkpy import sim_config
    assert play in PLAYS, play
    commit, sha = mod_identity(install_dir)
    return {"dump_sha256": dump_sha256(level), "mod_commit": commit, "mod_sha256": sha,
            "sim_keys": [[k, float(v)] for k, v in sim_config.sim_keys()], "game_env": sim_config.game_env(),
            "frames_per_wait": int(frames_per_wait), "recorder": recorder, "play": play}


def mod_mismatch(commit):
    """'' if a mod built from `commit` is the mod this checkout's oracle/ describes, else why not.
    Accepts a commit whose oracle/ differs from HEAD's when every changed path is in SAFE_ORACLE_DIFF
    (each provably cannot affect a recorded episode -- see its comment); refuses on any other oracle/
    change, exactly as an exact-match check would."""
    if not commit:
        return "mod build carries no commit"
    if commit.endswith("-dirty"):
        return "mod built from a dirty oracle/"
    files = _oracle_diff_files(commit)
    if files is None:
        return "mod commit %s is not in this repository" % commit[:10]
    unsafe = sorted(f for f in files if f not in SAFE_ORACLE_DIFF)
    if unsafe:
        return "oracle/ changed since mod commit %s (%s)" % (commit[:10], ", ".join(unsafe))
    return ""


def trace_header(path):
    """The JSON header of an .hktrace (docs/trace-format.md #Header), or {} if there is no such file."""
    if not os.path.exists(path):
        return {}
    with open(path, "rb") as fh:
        if fh.read(4) != b"HKTR":
            raise Refused("not an .hktrace: %s" % path)
        _ver, n = struct.unpack("<II", fh.read(8))
        return json.loads(fh.read(n).decode("utf-8"))


def legacy_reason(corpus, header):
    """-> "" when the game ran this corpus in the one configuration (hkpy/sim_config.py), else why it is
    LEGACY (module doc).  Raises Refused for a trace header outside regime R2, which the sim cannot run."""
    from hkpy import sim_config
    oenv = (header or {}).get("capture", {}).get("oracle_env")
    if oenv is not None:
        dt = (oenv.get("HK_ORACLE_CAPTURE_DT") or "").strip()
        bad = [("HK_ORACLE_CAPTURE_DT", dt)] if dt and dt != "0.02" else []
        bad += [(k, "0") for k in R2_OFF_IF_ZERO if (oenv.get(k) or "").strip() == "0"]
        if bad:
            raise Refused("%s=%s is outside regime R2, which is all the sim models" % bad[0])
        on = ["%s=%s" % (k, oenv[k]) for k in RETIRED_SWITCHES if oenv.get(k)]
        return ("recorded by a mod with the retired opt-in switches (%s); the sim replays it in the one "
                "configuration" % (", ".join(on) if on else "all off"))
    pv = corpus.get("provenance")
    if not pv:
        return ("no provenance stamp: its configuration is unknown (the mods before the stamp had opt-in "
                "switches); the sim replays it in the one configuration")
    keys = [(k, float(v)) for k, v in pv.get("sim_keys", [])]
    if keys != sim_config.sim_keys() or pv.get("game_env", {}) != sim_config.game_env():
        return ("stamped in a retired configuration (sim_keys %s, game_env %s); the sim replays it in the one "
                "configuration" % (dict(keys), pv.get("game_env", {})))
    return ""


def play(corpus, header):
    """-> ("policy" | "script" | "unknown", why): what chose the actions.  Stamped: the stamp's `play`.
    Legacy ws: a policy (GameFleet is driven by a checkpoint).  Legacy script: the action head decides.
    Every legacy policy corpus comes from a policy that never selected a hold action (it logs
    nail_charge / focus / dream_nail / super_dash at 0.000 each); a scripted or random generator uses them.
    The kind matters because a scripted corpus explores fewer boss states and diverges later, so its
    number is inflated, not merely different."""
    pv = corpus.get("provenance")
    if pv:
        p = pv.get("play")
        return (p, "stamp") if p in PLAYS else ("unknown", "the stamp names no play")
    if (header or {}).get("capture", {}).get("mode") == "ws":
        return "policy", "legacy ws recording"
    steps = corpus.get("steps") or []
    if not steps:
        return "unknown", "no steps"
    acts = [st["action"] if isinstance(st, dict) else st for st in steps]
    rate = sum(1 for a in acts if a[2] in HOLD_ACTIONS) / len(acts)
    if rate > 0.01:
        return "script", "%.1f%% of steps use a hold action (a legacy policy uses 0.000%%)" % (rate * 100)
    return "policy", "legacy script recording with no hold action"


def check_corpus(corpus, header=None):
    """-> (status, reason) for one loaded corpus.json and its trace header: status ok | refuse | legacy."""
    try:
        why = legacy_reason(corpus, header)
    except Refused as e:
        return "refuse", str(e)
    if why:
        return "legacy", why
    pv = corpus["provenance"]
    want = dump_sha256(corpus["level"])
    if pv.get("dump_sha256") != want:
        return "refuse", "recorded against dump %s, the sim is generated from %s" % (
            str(pv.get("dump_sha256"))[:10], str(want)[:10])
    why = mod_mismatch(pv.get("mod_commit", ""))
    if why:
        return "refuse", why
    if int(pv.get("frames_per_wait", -1)) != int(corpus.get("frames_per_wait", -2)):
        return "refuse", "stamp fpw %s != corpus fpw %s" % (pv.get("frames_per_wait"), corpus.get("frames_per_wait"))
    return "ok", ""


def _corpora(corpus_dir):
    """[(name, corpus, trace header)] for every <name>.corpus.json of a directory."""
    out = []
    for f in sorted(os.listdir(corpus_dir)):
        if f.endswith(".corpus.json"):
            n = f[:-len(".corpus.json")]
            with open(os.path.join(corpus_dir, f), encoding="utf-8") as fh:
                c = json.load(fh)
            out.append((n, c, trace_header(os.path.join(corpus_dir, n + ".a.hktrace"))))
    return out


def check(corpus_dir):
    """-> (status, reason) for a corpus directory: the worst status of its corpora (refuse > legacy > ok),
    and refuse when stamped corpora in it disagree on dump, mod or configuration."""
    stamps, worst, why = set(), "ok", ""
    rank = {"ok": 0, "legacy": 1, "refuse": 2}
    for n, c, h in _corpora(corpus_dir):
        st, r = check_corpus(c, h)
        if rank[st] > rank[worst]:
            worst, why = st, "%s: %s" % (n, r)
        pv = c.get("provenance")
        if pv:
            stamps.add(json.dumps({k: pv.get(k) for k in ("dump_sha256", "mod_commit", "sim_keys")},
                                  sort_keys=True))
    if len(stamps) > 1:
        return "refuse", "corpora in this directory carry %d different stamps" % len(stamps)
    return worst, why


def dir_play(corpus_dir):
    """-> ("policy" | "script" | "unknown", why) for a corpus directory: policy only if every corpus is."""
    for n, c, h in _corpora(corpus_dir):
        p, why = play(c, h)
        if p != "policy":
            return p, "%s: %s" % (n, why)
    return "policy", ""
