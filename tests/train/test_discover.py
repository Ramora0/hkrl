"""Properties of the discovery fit's loss (train_discover.subtb), on random
walks -- no sim, no GPU.

  1. The vectorized sub-trajectory balance equals a double loop over every
     piece (i, j) of every walk, weights lambda^(j-i), log F = 0 at a walk's
     end -- with returns and flows of the run's size (beta ~1000).
  2. The whole-line gap is trajectory balance: log F(s_0) + sum(log pi) -
     beta * R.

    python tests/train/test_discover.py      (or pytest tests/train)
"""
import numpy as np
import torch

import _paths  # noqa: F401
from train_discover import subtb


def brute(lp, u, d, n, beta, lam):
    logF = [beta * u[k] for k in range(n)] + [0.0]
    num = den = 0.0
    for i in range(n):
        for j in range(i + 1, n + 1):
            delta = logF[i] + sum(lp[k] - beta * d[k] for k in range(i, j)) - logF[j]
            num += lam ** (j - i) * delta ** 2
            den += lam ** (j - i)
    return num / den, logF[0] + sum(lp[:n]) - beta * sum(d[:n])


def test_subtb_matches_double_loop():
    rng = np.random.default_rng(0)
    B, L = 5, 230
    length = np.array([230, 1, 70, 150, 2])
    lp = -rng.random((B, L)) * 3
    u = rng.standard_normal((B, L))
    d = (rng.random((B, L)) < 0.2) * rng.random((B, L))
    for b in range(B):
        lp[b, length[b]:] = u[b, length[b]:] = d[b, length[b]:] = 0.0
    for beta, lam, scale in ((1.7, 0.9, 1.0), (1000.0, 0.99, 30.0)):
        uu = u * scale
        loss, whole = subtb(torch.tensor(lp), torch.tensor(uu), torch.tensor(d), torch.tensor(length),
                            beta, lam)
        for b in range(B):
            want, want_whole = brute(lp[b], uu[b], d[b], length[b], beta, lam)
            assert abs(float(loss[b]) - want) < 1e-7 * max(1.0, want), (beta, b, float(loss[b]), want)
            assert abs(float(whole[b]) - want_whole) < 1e-6 * max(1.0, abs(want_whole)), (b, float(whole[b]), want_whole)
    print(f"  subtb == double loop over every piece (beta 1.7 and 1000), lengths {length.tolist()}")


if __name__ == "__main__":
    test_subtb_matches_double_loop()
    print("PASS")
