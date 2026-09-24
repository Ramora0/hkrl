"""The configuration the sim and the game play in, for every caller: train/sim_worker.py (EpisodeStart),
train/game_eval.py (the fleet's environment), the corpus recorders (hkpy/provenance.py stamps it), every gate
replay (sim_driver.run_corpus) and every random-play soak (gate/boss_gate.py, gate/scenes_probe.py,
tools/soak.py, tools/fingerprint.py's fast path).  A caller that configures the sim any other way measures a
sim the policy never trains in.

The sim and the mod have one configuration (docs/sim-api.md "One configuration"): the hold table, focus on
Cast, armed rows and pool clones are built into both, and so is regime R2.  There is no hksim key to set and
no HKOracle switch to pass, so sim_keys() and game_env() are empty.  The trainer's per-episode setup
(hero.pd.maxHealth, hero.pd.health, hp.resync) is not configuration.
"""


def sim_keys():
    """[(hksim key, value)] an instance gets once, after its first hksim_reset: none."""
    return []


def game_env():
    """{HKOracle environment variable: value} a game process gets on top of its launch mode's own: none."""
    return {}


def apply(lib, sim):
    """hksim_set_value for each of sim_keys(); call after hksim_reset (the keys persist across resets)."""
    for key, value in sim_keys():
        if lib.hksim_set_value(sim, key.encode(), float(value)) != 0:
            raise RuntimeError("hksim_set_value(%s, %s) failed" % (key, value))
