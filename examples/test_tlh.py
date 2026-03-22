"""taxfolio tax-loss harvesting demo.

Usage:
    cd taxfolio
    uv run --no-project python examples/test_tlh.py
"""

import sys
from pathlib import Path

_root = str(Path(__file__).resolve().parents[1])
for p in [_root, str(Path(_root) / "build")]:
    if p not in sys.path:
        sys.path.insert(0, p)

import taxfolio as tf

# Portfolio with mix of gains and losses
portfolio = tf.Portfolio(
    holdings=[
        # Winners
        tf.Holding("NVDA", 50, 130.0, "2024-06-01"),
        tf.Holding("META", 40, 350.0, "2024-03-01"),
        tf.Holding("AMZN", 30, 150.0, "2024-01-15"),

        # Losers — TLH candidates
        tf.Holding("INTC", 200, 45.0, "2024-06-01"),     # Semiconductors
        tf.Holding("NKE", 100, 110.0, "2024-03-01"),      # Footwear
        tf.Holding("PFE", 300, 35.0, "2024-06-15"),       # Pharma
        tf.Holding("BA", 20, 250.0, "2024-09-01"),        # Aerospace

        # Neutral
        tf.Holding("AAPL", 80, 190.0, "2024-01-01"),
        tf.Holding("MSFT", 40, 400.0, "2024-06-01"),
    ],
    cash=5000,
)

print(f"Portfolio: {len(portfolio.holdings)} holdings\n")

# Tax-loss harvest
harvest = portfolio.harvest_losses(min_loss_pct=5.0, max_positions=5)
harvest.summary()
