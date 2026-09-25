"""Trajectory balance for a sampler, from onezero (C:/Users/Lee/coding/python/
AI/onezero, onezero/flow.py, whose docstring has the full argument): a line's
probability should be proportional to exp(beta * R),

    log P(line) = beta * R(line) - log Z,

and the gap delta = log P - beta * R - c, c the leave-one-out mean of the
line's group (VarGrad), is how far a line is from balance. Lines are drawn in
proportion to |gap| and weighted by its inverse (an unbiased estimate of the
gradient over every walk), and each step recomputes log P of whole lines
under the network being trained, so a line being pushed away stops when it
arrives.
"""
import numpy as np


def inverse_temperature(logu, lo, hi, scale):
    """scale * (entropy of uniform play over a walk) / (span of returns seen)."""
    span = hi - lo
    if not np.isfinite(span) or span <= 0 or np.size(logu) == 0:
        return 0.0
    return float(scale * np.mean(-np.asarray(logu)) / span)


def gaps(value, logp, group, beta):
    """delta per walk against the mean of the others in its group (-1: the
    root). A walk alone in its group reads 0."""
    x = np.asarray(logp, np.float64) - beta * np.asarray(value, np.float64)
    g = np.asarray(group, np.int64) + 1
    count = np.bincount(g)
    total = np.bincount(g, weights=x)
    n = count[g]
    rest = (total[g] - x) / np.maximum(n - 1, 1)
    return np.where(n > 1, x - rest, 0.0)


def reaches(delta, bound):
    """Huber's derivative: the gap, held to the bound."""
    if bound <= 0:
        return np.zeros_like(delta)
    return np.clip(delta, -bound, bound)


def draw_lines(reach, n, rng):
    """n lines drawn in proportion to |reach|, with replacement."""
    mass = np.abs(reach)
    total = mass.sum()
    if n <= 0 or total <= 0:
        return np.zeros(0, np.int64)
    return rng.choice(reach.size, size=n, p=mass / total)
