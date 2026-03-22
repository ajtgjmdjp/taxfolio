"""OptimizationResult — wrapper around the C++ RebalanceResult."""

from __future__ import annotations

import dataclasses
from typing import Any, Dict, List

try:
    import pandas as pd
except ImportError:  # pandas is optional
    pd = None  # type: ignore[assignment]


@dataclasses.dataclass
class TradeRecord:
    """A single recommended trade."""

    ticker: str
    side: str  # "buy" or "sell"
    shares: float
    notional: float

    def to_dict(self) -> Dict[str, Any]:
        return dataclasses.asdict(self)


@dataclasses.dataclass
class LotDispositionRecord:
    """Tax-lot disposition detail."""

    lot_id: int
    ticker: str
    shares_sold: float
    cost_basis: float
    proceeds: float
    realized_gain: float
    tax_character: str  # "short_term" or "long_term"
    tax_liability: float

    def to_dict(self) -> Dict[str, Any]:
        return dataclasses.asdict(self)


@dataclasses.dataclass
class OptimizationResult:
    """Result of a portfolio optimization.

    Attributes
    ----------
    trade_records : list[TradeRecord]
        Recommended trades.
    lot_dispositions : list[LotDispositionRecord]
        Per-lot tax disposition details (if any lots are sold).
    target_weights : list[float]
        Optimised target weight for each asset.
    converged : bool
        Whether the solver converged.
    solver_status : str
        Raw solver status string.
    objective_value : float
        Final objective value.
    allocation_drift : float
        Ex-ante tracking error.
    turnover : float
        Total turnover (one-way).
    estimated_tax_cost : float
        Estimated dollar tax cost.
    estimated_transaction_cost : float
        Estimated dollar transaction cost.
    """

    trade_records: List[TradeRecord]
    lot_dispositions: List[LotDispositionRecord]
    target_weights: List[float]
    converged: bool
    solver_status: str
    objective_value: float
    allocation_drift: float
    turnover: float
    estimated_tax_cost: float
    estimated_transaction_cost: float

    # -- convenience properties ------------------------------------------------

    @property
    def trades(self) -> Any:
        """Return trades as a :class:`pandas.DataFrame` if pandas is installed,
        otherwise a list of dicts."""
        records = [t.to_dict() for t in self.trade_records]
        if pd is not None:
            return pd.DataFrame(records) if records else pd.DataFrame(
                columns=["ticker", "side", "shares", "notional"]
            )
        return records

    @property
    def tax_savings(self) -> float:
        """Estimated tax savings (positive = savings from loss harvesting).

        This is the negative of the estimated tax cost, so a positive value
        means the optimizer harvested losses to reduce the tax bill.
        """
        return -self.estimated_tax_cost

    # -- summary ---------------------------------------------------------------

    def summary(self, print_output: bool = True) -> str:
        """Return (and optionally print) a human-readable summary."""
        lines = [
            "=== taxfolio Optimization Result ===",
            f"Solver status   : {self.solver_status} ({'converged' if self.converged else 'NOT converged'})",
            f"Alloc. drift    : {self.allocation_drift:.4%}",
            f"Turnover        : {self.turnover:.4%}",
            f"Tax cost (est.) : ${self.estimated_tax_cost:,.2f}",
            f"Txn cost (est.) : ${self.estimated_transaction_cost:,.2f}",
            f"Tax savings     : ${self.tax_savings:,.2f}",
            "",
            f"Trades ({len(self.trade_records)}):",
        ]
        for t in self.trade_records:
            lines.append(
                f"  {t.side.upper():4s} {t.ticker:>8s}  "
                f"{t.shares:>10.2f} shares  ${t.notional:>12,.2f}"
            )
        if self.lot_dispositions:
            lines.append("")
            lines.append(f"Lot dispositions ({len(self.lot_dispositions)}):")
            for ld in self.lot_dispositions:
                lines.append(
                    f"  Lot {ld.lot_id}: {ld.ticker} "
                    f"sell {ld.shares_sold:.2f} sh  "
                    f"gain=${ld.realized_gain:,.2f} ({ld.tax_character})  "
                    f"tax=${ld.tax_liability:,.2f}"
                )
        text = "\n".join(lines)
        if print_output:
            print(text)
        return text

    def __repr__(self) -> str:
        return (
            f"OptimizationResult(trades={len(self.trade_records)}, "
            f"converged={self.converged}, "
            f"tax_cost={self.estimated_tax_cost:.2f})"
        )
