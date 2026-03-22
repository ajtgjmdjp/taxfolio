"""taxfolio quickstart — automatic tax-loss harvesting.

Usage:
    cd taxfolio
    uv run --no-project python examples/quickstart.py
"""

import sys
from pathlib import Path

_root = str(Path(__file__).resolve().parents[1])
for p in [_root, str(Path(_root) / "build")]:
    if p not in sys.path:
        sys.path.insert(0, p)

import taxfolio as tf

# Portfolio with winners and losers
portfolio = tf.Portfolio(
    holdings=[
        tf.Holding("AAPL", 100, 170.00, "2024-01-15"),   # winner
        tf.Holding("NVDA", 40, 50.00, "2023-06-01"),      # big winner
        tf.Holding("INTC", 300, 48.00, "2024-06-01"),     # loser
        tf.Holding("PFE", 400, 32.00, "2024-03-01"),      # loser
        tf.Holding("NKE", 150, 105.00, "2024-03-01"),     # loser
        tf.Holding("BA", 30, 240.00, "2024-09-01"),       # loser
    ],
    cash=0,
)

# One line: find losses, match substitutes, generate trades
harvest = portfolio.harvest_losses(min_loss_pct=5.0)
harvest.summary()
