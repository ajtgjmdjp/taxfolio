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

__version__ = "0.2.0"

from .holding import Holding
from .portfolio import Portfolio
from .harvest import HarvestResult, HarvestTrade
from .importers import from_schwab, from_fidelity, from_ibkr
from .result import LotDispositionRecord, OptimizationResult, TradeRecord

__all__ = [
    "HarvestResult",
    "HarvestTrade",
    "Holding",
    "LotDispositionRecord",
    "OptimizationResult",
    "Portfolio",
    "TradeRecord",
    "from_schwab",
    "from_fidelity",
    "from_ibkr",
    "__version__",
]
