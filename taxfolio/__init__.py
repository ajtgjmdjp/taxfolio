"""taxfolio — tax-aware portfolio optimisation.

>>> import taxfolio as tf
>>> p = tf.Portfolio(
...     holdings=[tf.Holding("AAPL", 100, 150.0, "2025-01-15")],
...     cash=5000,
... )
>>> result = p.optimize(tax_jurisdiction="us")
>>> print(result.summary())
"""

from __future__ import annotations

__version__ = "0.1.0a0"

from .holding import Holding
from .portfolio import Portfolio
from .harvest import HarvestResult, HarvestTrade
from .result import LotDispositionRecord, OptimizationResult, TradeRecord

__all__ = [
    "HarvestResult",
    "HarvestTrade",
    "Holding",
    "LotDispositionRecord",
    "OptimizationResult",
    "Portfolio",
    "TradeRecord",
    "__version__",
]
