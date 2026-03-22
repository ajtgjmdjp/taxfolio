"""Holding dataclass — a single position in a portfolio."""

from __future__ import annotations

import dataclasses
from datetime import date
from typing import Optional


@dataclasses.dataclass(frozen=True)
class Holding:
    """A single holding in a portfolio.

    Parameters
    ----------
    ticker : str
        Ticker symbol (e.g. ``"AAPL"``, ``"7203.T"``).
    shares : float
        Number of shares held.
    cost_basis : float
        Per-share cost basis in local currency.
    acquired : str | date
        Acquisition date as ``"YYYY-MM-DD"`` string or :class:`datetime.date`.
    lot_id : int | None
        Optional lot identifier.  Auto-assigned when omitted.
    """

    ticker: str
    shares: float
    cost_basis: float
    acquired: str | date
    lot_id: Optional[int] = None

    # -- helpers ---------------------------------------------------------------

    @property
    def acquired_str(self) -> str:
        """Return the acquisition date as a ``YYYY-MM-DD`` string."""
        if isinstance(self.acquired, date):
            return self.acquired.isoformat()
        return str(self.acquired)

    @property
    def is_japan(self) -> bool:
        """Heuristic: tickers ending with ``.T`` are Japanese equities."""
        return self.ticker.upper().endswith(".T")

    def __repr__(self) -> str:
        return (
            f"Holding({self.ticker!r}, shares={self.shares}, "
            f"cost_basis={self.cost_basis}, acquired={self.acquired_str!r})"
        )
