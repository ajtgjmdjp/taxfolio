"""taxfolio large portfolio demo — 30 stocks with realistic gains/losses.

Usage:
    cd taxfolio
    uv run --no-project python examples/large_portfolio.py
"""

import sys
from pathlib import Path

_root = str(Path(__file__).resolve().parents[1])
for p in [_root, str(Path(_root) / "build")]:
    if p not in sys.path:
        sys.path.insert(0, p)

import taxfolio as tf

# 30-stock portfolio: diverse sectors, mix of gains and losses
portfolio = tf.Portfolio(
    holdings=[
        # Tech — winners
        tf.Holding("AAPL", 100, 170.00, "2024-01-15"),
        tf.Holding("NVDA", 40, 50.00, "2023-06-01"),
        tf.Holding("META", 60, 300.00, "2024-03-01"),
        tf.Holding("GOOGL", 50, 140.00, "2024-02-01"),

        # Tech — losers
        tf.Holding("INTC", 300, 48.00, "2024-06-01"),
        tf.Holding("CSCO", 200, 55.00, "2024-04-01"),
        tf.Holding("IBM", 50, 200.00, "2024-08-01"),

        # Healthcare — mixed
        tf.Holding("JNJ", 80, 165.00, "2024-01-01"),
        tf.Holding("PFE", 400, 32.00, "2024-03-01"),
        tf.Holding("UNH", 20, 550.00, "2024-06-01"),
        tf.Holding("ABT", 60, 120.00, "2024-05-01"),

        # Finance — mixed
        tf.Holding("JPM", 50, 180.00, "2024-02-01"),
        tf.Holding("V", 30, 280.00, "2024-04-01"),
        tf.Holding("BAC", 200, 38.00, "2024-07-01"),
        tf.Holding("BRK-B", 25, 380.00, "2024-01-15"),

        # Consumer — losers
        tf.Holding("NKE", 150, 105.00, "2024-03-01"),
        tf.Holding("KO", 100, 62.00, "2024-06-01"),
        tf.Holding("PG", 40, 165.00, "2024-05-01"),
        tf.Holding("MCD", 30, 290.00, "2024-04-01"),

        # Energy
        tf.Holding("XOM", 80, 110.00, "2024-01-01"),
        tf.Holding("CVX", 60, 155.00, "2024-03-01"),

        # Industrials — losers
        tf.Holding("BA", 30, 240.00, "2024-09-01"),
        tf.Holding("CAT", 25, 350.00, "2024-06-01"),
        tf.Holding("GE", 40, 180.00, "2024-08-01"),

        # Misc
        tf.Holding("AMZN", 30, 170.00, "2024-02-01"),
        tf.Holding("NFLX", 20, 700.00, "2024-04-01"),
        tf.Holding("WMT", 50, 170.00, "2024-03-01"),
        tf.Holding("HD", 25, 370.00, "2024-05-01"),
        tf.Holding("COST", 15, 750.00, "2024-06-01"),
        tf.Holding("DIS", 100, 110.00, "2024-07-01"),
    ],
    cash=0,
)

print(f"Portfolio: {len(portfolio.holdings)} holdings\n")

# Tax-Loss Harvesting
print("=" * 60)
print("TAX-LOSS HARVESTING (top 10 opportunities)")
print("=" * 60)
harvest = portfolio.harvest_losses(min_loss_pct=5.0, max_positions=10)
harvest.summary()
