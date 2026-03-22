"""Engine — bridge between the Python API and the C++ ``_taxfolio`` module."""

from __future__ import annotations

import os
import sys
from datetime import date
from typing import Dict, List, Optional, Sequence

import numpy as np

from .holding import Holding
from .result import LotDispositionRecord, OptimizationResult, TradeRecord

# ---------------------------------------------------------------------------
# Lazy-load the C++ extension from ``build/``
# ---------------------------------------------------------------------------

_cpp: Optional[object] = None


def _load_cpp():  # noqa: ANN202
    """Import ``_taxfolio`` from the ``build/`` directory at the repo root."""
    global _cpp
    if _cpp is not None:
        return _cpp

    # Resolve: <repo>/taxfolio/engine.py -> <repo>/build
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = os.path.join(repo_root, "build")

    if build_dir not in sys.path:
        sys.path.insert(0, build_dir)

    import _taxfolio  # type: ignore[import-not-found]

    _cpp = _taxfolio
    return _cpp


# ---------------------------------------------------------------------------
# Tax jurisdiction presets
# ---------------------------------------------------------------------------

_TAX_PRESETS: Dict[str, Dict[str, object]] = {
    "us": {
        "long_term_rate": 0.20,
        "short_term_rate": 0.37,
        "wash_sale_window_days": 30,
    },
    "japan": {
        "long_term_rate": 0.20315,
        "short_term_rate": 0.20315,
        "wash_sale_window_days": 0,
    },
}


# ---------------------------------------------------------------------------
# Risk model builders
# ---------------------------------------------------------------------------


def _fetch_covariance(
    asset_ids: List[str],
    period: str = "1y",
    method: str = "sample",
) -> np.ndarray:
    """Fetch historical prices via yfinance and compute annualized covariance.

    Parameters
    ----------
    method : str
        ``"sample"`` for standard sample covariance,
        ``"ewma"`` for exponentially weighted (span=60 days),
        ``"shrinkage"`` for Ledoit-Wolf shrinkage target (identity).
    """
    try:
        import yfinance as yf
        import pandas as pd

        data = yf.download(asset_ids, period=period, auto_adjust=True, progress=False)
        if isinstance(data.columns, pd.MultiIndex):
            close = data["Close"]
        else:
            close = data

        available = [t for t in asset_ids if t in close.columns and close[t].notna().sum() > 20]
        close = close[available].ffill().dropna()
        daily_ret = close.pct_change().dropna()

        if method == "ewma":
            cov_df = daily_ret.ewm(span=60).cov()
            # Extract last date's covariance matrix
            last_date = cov_df.index.get_level_values(0)[-1]
            cov_raw = cov_df.loc[last_date].values * 252
        elif method == "shrinkage":
            sample_cov = daily_ret.cov().values * 252
            n = sample_cov.shape[0]
            # Ledoit-Wolf shrinkage toward scaled identity
            mu = np.trace(sample_cov) / n
            delta = 0.2  # fixed shrinkage intensity
            cov_raw = (1 - delta) * sample_cov + delta * mu * np.eye(n)
        else:
            cov_raw = daily_ret.cov().values * 252

        if len(available) < len(asset_ids):
            n = len(asset_ids)
            cov = np.eye(n) * 0.04
            avail_idx = {t: i for i, t in enumerate(asset_ids)}
            for i, t1 in enumerate(available):
                for j, t2 in enumerate(available):
                    ci, cj = avail_idx[t1], avail_idx[t2]
                    cov[ci, cj] = cov_raw[i, j]
            return cov
        else:
            return cov_raw

    except Exception as e:
        import warnings
        warnings.warn(
            f"Failed to fetch covariance data from yfinance: {e}. "
            "Using diagonal fallback (20% annualized vol).",
            stacklevel=2,
        )
        return np.eye(len(asset_ids)) * 0.04


def _fetch_prices(asset_ids: List[str]) -> Dict[str, float]:
    """Fetch latest prices via yfinance."""
    try:
        import yfinance as yf
        import pandas as pd

        data = yf.download(asset_ids, period="5d", auto_adjust=True, progress=False)
        if isinstance(data.columns, pd.MultiIndex):
            close = data["Close"]
        else:
            close = data

        close = close.ffill().dropna()
        if len(close) == 0:
            raise RuntimeError("No price data returned from yfinance")

        latest = close.iloc[-1]
        prices = {t: float(latest[t]) for t in asset_ids if t in latest.index and not np.isnan(latest[t])}
        missing = [t for t in asset_ids if t not in prices]
        if missing:
            import warnings
            warnings.warn(f"No price data for: {', '.join(missing)}", stacklevel=2)
        return prices
    except Exception as e:
        raise RuntimeError(
            f"Failed to fetch prices from yfinance: {e}. "
            "Pass prices= explicitly to avoid network calls."
        ) from e


def _build_risk_model(
    kind: str,
    asset_ids: List[str],
    prices: np.ndarray,
    period: str = "1y",
) -> object:
    """Build covariance from real market data and wrap in C++ risk model."""
    cpp = _load_cpp()
    method = kind.lower() if kind.lower() in ("sample", "ewma", "shrinkage") else "sample"
    cov = _fetch_covariance(asset_ids, period=period, method=method)

    # Trace normalization (same as api.cpp)
    n = len(asset_ids)
    tr = np.trace(cov)
    if tr > 1e-12:
        cov = cov * (n / tr)

    return cpp.FullCovarianceRisk(asset_ids, cov)


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def run_optimization(
    holdings: Sequence[Holding],
    cash: float,
    *,
    as_of: Optional[str] = None,
    prices: Optional[Dict[str, float]] = None,
    tax_jurisdiction: str = "us",
    risk_model: str = "sample",
    lambda_drift: float = 50.0,
    lambda_tax: float = 400.0,
    lambda_txn: float = 1.0,
    max_turnover: float = 0.15,
    max_weight: float = 0.20,
    cash_buffer: float = 0.0,
    round_shares: bool = True,
    min_trade_notional: Optional[float] = None,
    solver: str = "osqp",
) -> OptimizationResult:
    """Run tax-aware portfolio optimisation via the C++ core.

    Parameters
    ----------
    holdings : sequence of Holding
        Current positions.
    cash : float
        Available cash.
    as_of : str, optional
        Valuation date (``YYYY-MM-DD``).  Defaults to today.
    prices : dict[str, float], optional
        Current market prices keyed by ticker.  If *None*, prices are
        approximated as ``cost_basis * 1.0`` (i.e. flat market assumption).
    tax_jurisdiction : str
        ``"us"`` or ``"japan"``.  Sets tax rates automatically.
    risk_model : str
        ``"sample"``, ``"ewma"``, ``"shrinkage"``, or ``"factor"``.
    lambda_drift : float
        Objective weight for tracking error (higher = penalise drift more).
    lambda_tax : float
        Objective weight for tax cost (higher = more tax-aware).
    lambda_txn : float
        Objective weight for transaction cost.
    max_turnover : float
        Maximum one-way turnover (0-1 fraction of portfolio value).
    round_shares : bool
        Whether to round trades to whole shares.
    min_trade_notional : float
        Minimum notional value for a trade to be executed.
    solver : str
        QP solver to use (``"osqp"`` or ``"scs"``).

    Returns
    -------
    OptimizationResult
    """
    cpp = _load_cpp()

    if as_of is None:
        as_of = date.today().isoformat()

    # -- Build asset list & price vector -----------------------------------
    asset_ids: List[str] = []
    seen: Dict[str, int] = {}
    for h in holdings:
        if h.ticker not in seen:
            seen[h.ticker] = len(asset_ids)
            asset_ids.append(h.ticker)

    n = len(asset_ids)

    # Fetch live prices if not provided
    if prices is None:
        prices = _fetch_prices(asset_ids)

    price_vec = np.zeros(n, dtype=np.float64)
    for i, aid in enumerate(asset_ids):
        if aid in prices:
            price_vec[i] = prices[aid]
        else:
            price_vec[i] = _avg_cost(holdings, aid)  # fallback to cost basis

    # -- Tax lots ----------------------------------------------------------
    lots = []
    lot_counter = 1
    for h in holdings:
        lid = h.lot_id if h.lot_id is not None else lot_counter
        lots.append(
            cpp.TaxLot(lid, h.ticker, float(h.shares), float(h.cost_basis), h.acquired_str)
        )
        lot_counter += 1

    portfolio = cpp.PortfolioState(as_of, float(cash), lots)

    # -- Market data -------------------------------------------------------
    # Self-benchmark: target weights = current portfolio weights
    total_value = cash
    for h in holdings:
        idx = seen[h.ticker]
        total_value += h.shares * price_vec[idx]

    benchmark_weights = np.zeros(n, dtype=np.float64)
    if total_value > 0:
        for h in holdings:
            idx = seen[h.ticker]
            benchmark_weights[idx] += h.shares * price_vec[idx] / total_value

    txn_cost_bps = np.full(n, 10.0, dtype=np.float64)  # 10 bps default

    risk = _build_risk_model(risk_model, asset_ids, price_vec)
    market = cpp.MarketData(as_of, asset_ids, price_vec, benchmark_weights, txn_cost_bps, risk)

    # -- Optimisation config -----------------------------------------------
    config = cpp.OptimizationConfig()
    config.objective.allocation_drift = lambda_drift
    config.objective.tax_cost = lambda_tax
    config.objective.transaction_cost = lambda_txn
    config.constraints.max_turnover = max_turnover
    config.constraints.cash_buffer = cash_buffer
    config.round_to_whole_shares = round_shares
    config.solver = solver

    # Auto-scale min_trade_notional to portfolio size (0.1%)
    config.min_trade_notional = min_trade_notional if min_trade_notional is not None else max(100, total_value * 0.001)

    # Per-asset weight bounds
    for i, aid in enumerate(asset_ids):
        config.constraints.weight_bounds[aid] = cpp.WeightBound(0.0, max_weight)

    # Tax config
    jurisdiction = tax_jurisdiction.lower()
    preset = _TAX_PRESETS.get(jurisdiction)
    if preset is None:
        raise ValueError(
            f"Unknown tax jurisdiction {tax_jurisdiction!r}. Choose from: us, japan."
        )
    config.taxes.long_term_rate = preset["long_term_rate"]  # type: ignore[assignment]
    config.taxes.short_term_rate = preset["short_term_rate"]  # type: ignore[assignment]
    config.taxes.wash_sale_window_days = preset["wash_sale_window_days"]  # type: ignore[assignment]
    config.taxes.allow_loss_harvesting = True
    config.taxes.disposal_method = cpp.DisposalMethod.specific_id

    # -- Run ---------------------------------------------------------------
    request = cpp.RebalanceRequest(portfolio, market, config)
    raw = cpp.plan_rebalance(request)

    # -- Convert result ----------------------------------------------------
    trade_records = []
    for t in raw.trades:
        trade_records.append(
            TradeRecord(
                ticker=t.asset_id,
                side=t.side.name,
                shares=t.shares,
                notional=t.notional,
            )
        )

    lot_disps = []
    for ld in raw.lot_dispositions:
        lot_disps.append(
            LotDispositionRecord(
                lot_id=ld.lot_id,
                ticker=ld.asset_id,
                shares_sold=ld.shares_sold,
                cost_basis=ld.cost_basis,
                proceeds=ld.proceeds,
                realized_gain=ld.realized_gain,
                tax_character=ld.tax_character.name,
                tax_liability=ld.tax_liability,
            )
        )

    diag = raw.diagnostics
    return OptimizationResult(
        trade_records=trade_records,
        lot_dispositions=lot_disps,
        target_weights=list(raw.target_weights),
        converged=diag.converged,
        solver_status=diag.solver_status,
        objective_value=diag.objective_value,
        allocation_drift=diag.ex_ante_allocation_drift,
        turnover=diag.turnover,
        estimated_tax_cost=diag.estimated_tax_cost,
        estimated_transaction_cost=diag.estimated_transaction_cost,
    )


def _avg_cost(holdings: Sequence[Holding], ticker: str) -> float:
    """Weighted-average cost basis for *ticker* across *holdings*."""
    total_shares = 0.0
    total_cost = 0.0
    for h in holdings:
        if h.ticker == ticker:
            total_shares += h.shares
            total_cost += h.shares * h.cost_basis
    if total_shares == 0:
        return 0.0
    return total_cost / total_shares
