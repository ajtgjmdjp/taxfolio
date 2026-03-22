"""Portfolio class — the main user-facing entry point."""

from __future__ import annotations

import csv
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Union

from .holding import Holding
from .result import OptimizationResult


class Portfolio:
    """A portfolio of holdings with optional cash.

    Parameters
    ----------
    holdings : sequence of Holding
        Current positions.
    cash : float
        Available cash (default 0).

    Examples
    --------
    >>> p = Portfolio(
    ...     holdings=[Holding("AAPL", 100, 150.0, "2025-01-15")],
    ...     cash=5000,
    ... )
    >>> result = p.optimize(tax_jurisdiction="us")
    >>> print(result.summary())
    """

    def __init__(
        self,
        holdings: Sequence[Holding],
        cash: float = 0.0,
    ) -> None:
        self.holdings: List[Holding] = list(holdings)
        self.cash: float = float(cash)

    # -- constructors ----------------------------------------------------------

    @classmethod
    def from_csv(
        cls,
        path: Union[str, Path],
        *,
        cash: float = 0.0,
        ticker_col: str = "ticker",
        shares_col: str = "shares",
        cost_basis_col: str = "cost_basis",
        acquired_col: str = "acquired",
    ) -> "Portfolio":
        """Load a portfolio from a CSV file.

        Expected columns (configurable via keyword arguments):

        - ``ticker`` — ticker symbol
        - ``shares`` — number of shares
        - ``cost_basis`` — per-share cost basis
        - ``acquired`` — acquisition date (``YYYY-MM-DD``)

        Parameters
        ----------
        path : str or Path
            Path to the CSV file.
        cash : float
            Starting cash balance.
        ticker_col, shares_col, cost_basis_col, acquired_col : str
            Column name overrides.
        """
        path = Path(path)
        holdings: List[Holding] = []

        # Column alias maps for flexible CSV parsing
        _ticker_aliases = {"ticker", "symbol", "code", "asset_id"}
        _shares_aliases = {"shares", "quantity", "qty", "units"}
        _cost_aliases = {"cost_basis", "cost_basis_per_share", "cost", "avg_cost", "price",
                         "cost/share", "cost_per_share", "unit_cost", "average_cost"}
        _date_aliases = {"acquired", "acquired_on", "purchase_date", "date",
                         "date_acquired", "acquisition_date", "open_date", "trade_date"}

        def _find_col(headers: list[str], preferred: str, aliases: set[str]) -> str:
            lower_map = {h.lower().strip(): h for h in headers}
            if preferred.lower() in lower_map:
                return lower_map[preferred.lower()]
            for alias in aliases:
                if alias in lower_map:
                    return lower_map[alias]
            return preferred  # fall back, will raise KeyError later

        with open(path, newline="", encoding="utf-8-sig") as fh:
            reader = csv.DictReader(fh)
            headers = list(reader.fieldnames or [])
            tc = _find_col(headers, ticker_col, _ticker_aliases)
            sc = _find_col(headers, shares_col, _shares_aliases)
            cc = _find_col(headers, cost_basis_col, _cost_aliases)
            ac = _find_col(headers, acquired_col, _date_aliases)

            for i, row in enumerate(reader, start=1):
                try:
                    holdings.append(
                        Holding(
                            ticker=row[tc].strip(),
                            shares=float(row[sc]),
                            cost_basis=float(row[cc]),
                            acquired=row[ac].strip(),
                            lot_id=i,
                        )
                    )
                except KeyError as exc:
                    raise ValueError(
                        f"Missing column {exc} in CSV (row {i}). "
                        f"Available: {headers}"
                    ) from exc
        return cls(holdings=holdings, cash=cash)

    # -- optimisation ----------------------------------------------------------

    def optimize(
        self,
        *,
        tax_jurisdiction: Optional[str] = None,
        risk_model: str = "sample",
        lambda_drift: float = 50.0,
        lambda_tax: float = 400.0,
        lambda_txn: float = 1.0,
        prices: Optional[Dict[str, float]] = None,
        as_of: Optional[str] = None,
        max_turnover: float = 0.15,
        max_weight: float = 0.20,
        cash_buffer: float = 0.0,
        round_shares: bool = True,
        min_trade_notional: Optional[float] = None,
        solver: str = "osqp",
    ) -> OptimizationResult:
        """Run tax-aware portfolio optimisation.

        Parameters
        ----------
        tax_jurisdiction : str, optional
            ``"us"`` or ``"japan"``.  If *None*, auto-detected from ticker
            suffixes (any ``.T`` ticker triggers ``"japan"``).
        risk_model : str
            ``"sample"``, ``"ewma"``, ``"shrinkage"``, or ``"factor"``.
        lambda_drift : float
            Objective weight for tracking error.
        lambda_tax : float
            Objective weight for tax cost.
        lambda_txn : float
            Objective weight for transaction cost.
        prices : dict, optional
            Current prices ``{ticker: price}``.  If omitted, cost basis is
            used as a flat-market approximation.
        as_of : str, optional
            Valuation date.  Defaults to today.
        max_turnover : float
            Maximum turnover fraction (0-1).
        round_shares : bool
            Round trades to whole shares.
        min_trade_notional : float
            Minimum notional for a trade.
        solver : str
            QP solver (``"osqp"`` or ``"scs"``).

        Returns
        -------
        OptimizationResult
        """
        from .engine import run_optimization

        if tax_jurisdiction is None:
            tax_jurisdiction = self._detect_jurisdiction()

        return run_optimization(
            holdings=self.holdings,
            cash=self.cash,
            as_of=as_of,
            prices=prices,
            tax_jurisdiction=tax_jurisdiction,
            risk_model=risk_model,
            lambda_drift=lambda_drift,
            lambda_tax=lambda_tax,
            lambda_txn=lambda_txn,
            max_turnover=max_turnover,
            max_weight=max_weight,
            cash_buffer=cash_buffer,
            round_shares=round_shares,
            min_trade_notional=min_trade_notional,
            solver=solver,
        )

    # -- tax-loss harvesting ---------------------------------------------------

    def harvest_losses(
        self,
        *,
        prices: Optional[Dict[str, float]] = None,
        min_loss_pct: float = 5.0,
        min_correlation: float = 0.5,
        max_positions: int = 10,
        tax_rate: Optional[float] = None,
        wash_sale_days: Optional[int] = None,
        recent_sells: Optional[Dict[str, str]] = None,
    ):
        """Identify tax-loss harvesting opportunities with automatic substitute matching.

        Parameters
        ----------
        min_loss_pct : float
            Minimum unrealized loss percentage (e.g., 5.0 = positions down 5%+).
        min_correlation : float
            Minimum return correlation for substitute securities (0-1).
        max_positions : int
            Maximum number of positions to harvest.
        wash_sale_days : int, optional
            Wash sale window in days. Default: 30 (US) or 0 (Japan).
        recent_sells : dict, optional
            {ticker: "YYYY-MM-DD"} for wash sale detection.

        Returns a HarvestResult with recommended sell/buy pairs.
        """
        from .harvest import harvest_losses as _harvest

        is_japan = self._detect_jurisdiction() == "japan"
        if tax_rate is None:
            tax_rate = 0.20315 if is_japan else 0.37
        if wash_sale_days is None:
            wash_sale_days = 0 if is_japan else 30

        return _harvest(
            holdings=self.holdings,
            cash=self.cash,
            prices=prices,
            min_loss_pct=min_loss_pct,
            min_correlation=min_correlation,
            max_positions=max_positions,
            tax_rate=tax_rate,
            wash_sale_days=wash_sale_days,
            recent_sells=recent_sells,
        )

    # -- helpers ---------------------------------------------------------------

    def _detect_jurisdiction(self) -> str:
        """Auto-detect tax jurisdiction from ticker suffixes."""
        for h in self.holdings:
            if h.is_japan:
                return "japan"
        return "us"

    @property
    def tickers(self) -> List[str]:
        """Unique tickers in the portfolio."""
        seen: Dict[str, None] = {}
        for h in self.holdings:
            seen.setdefault(h.ticker, None)
        return list(seen)

    @property
    def total_shares(self) -> Dict[str, float]:
        """Aggregate shares per ticker."""
        agg: Dict[str, float] = {}
        for h in self.holdings:
            agg[h.ticker] = agg.get(h.ticker, 0.0) + h.shares
        return agg

    def __repr__(self) -> str:
        n_holdings = len(self.holdings)
        n_tickers = len(self.tickers)
        return (
            f"Portfolio({n_holdings} holding(s) across {n_tickers} ticker(s), "
            f"cash={self.cash:,.2f})"
        )

    def __len__(self) -> int:
        return len(self.holdings)
