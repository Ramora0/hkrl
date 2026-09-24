"""tools/attack_gap.py known answers: the one hit attribution both evals call, and the per-attack gap table
flipping when a hit moves from one attack to another or loses its source.

    pytest tests/test_attack_gap.py
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import attack_gap  # noqa: E402


def rows(*specs):
    """(rel_x, w, gives_damage, is_target) -> combat rows 1x1 high at rel_y 0."""
    c = np.zeros((len(specs), 14), np.float32)
    for i, (x, w, dmg, tgt) in enumerate(specs):
        c[i, 0], c[i, 2], c[i, 3], c[i, 7], c[i, 9] = x, w, 1.0, dmg, tgt
    return c


def test_attack_gap_attribution():
    gs = np.zeros(33, np.float32)
    gs[4], gs[5] = 0.5, 1.0
    c = rows((3.0, 1.0, 1, 1), (1.0, 0.5, 1, 0), (0.2, 1.0, 0, 0))
    got = attack_gap.attribute(c, ["Boss", "Spear", "Wall"], ["Boss|Throw", "Spear|Fly", "Wall|none"], gs)
    assert got == ["Spear", "Spear|Fly", 0.5, "Throw", ""]
    assert attack_gap.attribute(np.zeros((0, 14)), [], [], gs) == ["", "", 99.0, "", ""]


def test_attack_gap_table_flips():
    g = [{"hit_by": [[1, "Spear", "", 0.0, "Throw"], [2, "Boss", "", 0.0, "Dash"]]},
         {"hit_by": [[1, "Spear", "", 0.0, "Throw"]]}]
    s = [{"hit_by": [[1, "Boss", "", 0.0, "Dash"]]}, {"hit_by": []}]
    keyed, t = attack_gap.table(g, s)
    assert keyed.startswith("nearest") and t[0] == ("Spear | boss Throw", 1.0, 0.0)
    s2 = [{"hit_by": [[1, "Spear", "", 0.0, "Throw"], [2, "Spear", "", 0.0, "Throw"]]}, {"hit_by": []}]
    _k, t2 = attack_gap.table(g, s2)
    assert t2[0] == ("Boss | boss Dash", 0.5, 0.0) and ("Spear | boss Throw", 1.0, 1.0) in t2
    # sources on every hit of both sides: keyed by source; one hit without one: the row key for all
    gs_ = [{"hit_by": [[1, "Boss", "", 0.0, "Slash", "Slash2"]]}]
    ss_ = [{"hit_by": [[1, "Boss", "", 0.0, "Slash", "Nightmare Grimm Boss"]]}]
    k3, t3 = attack_gap.table(gs_, ss_)
    assert k3 == "HERO_DAMAGE source" and {r[0] for r in t3} == {"Slash2", "Nightmare Grimm Boss"}
    k4, t4 = attack_gap.table(gs_, [{"hit_by": [[1, "Boss", "", 0.0, "Slash"]]}])
    assert k4.startswith("nearest") and t4 == [("Boss | boss Slash", 1.0, 1.0)]
