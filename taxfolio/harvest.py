"""Tax-loss harvesting — detect losses, find correlated substitutes, generate trades."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path
from typing import Dict, List, Optional, Sequence


from .holding import Holding

# ---------------------------------------------------------------------------
# Substitute security map + correlation cache
# ---------------------------------------------------------------------------

_SUBSTITUTES: Optional[Dict[str, List[str]]] = None
_CORR_CACHE: Dict[tuple[str, str], float] = {}


def _load_substitutes() -> Dict[str, List[str]]:
    global _SUBSTITUTES
    if _SUBSTITUTES is not None:
        return _SUBSTITUTES
    path = Path(__file__).parent / "data" / "substitutes_us.json"
    with open(path) as f:
        raw = json.load(f)
    _SUBSTITUTES = {k: v for k, v in raw.items() if not k.startswith("_")}
    return _SUBSTITUTES


def _get_candidates(ticker: str, exclude: set[str]) -> list[tuple[str, str]]:
    """Get candidate substitutes from the static map.

    Returns list of (candidate_ticker, industry_name).
    """
    subs = _load_substitutes()
    results = []
    for industry, members in subs.items():
        if ticker in members:
            for candidate in members:
                if candidate != ticker and candidate not in exclude:
                    results.append((candidate, industry))
    if not results:
        # Fallback: yfinance industry lookup
        try:
            import yfinance as yf
            info = yf.Ticker(ticker).info
            yf_industry = info.get("industry", "")
            for map_industry, members in subs.items():
                if map_industry.lower() in yf_industry.lower() or yf_industry.lower() in map_industry.lower():
                    for candidate in members:
                        if candidate != ticker and candidate not in exclude:
                            results.append((candidate, yf_industry))
        except Exception:
            pass
    return results


def _compute_correlations_batch(
    source_ticker: str,
    candidates: list[str],
    period: str = "1y",
) -> Dict[str, float]:
    """Compute return correlations between source and multiple candidates in one download."""

    # Check cache first
    results = {}
    to_fetch = []
    for c in candidates:
        cache_key = (min(source_ticker, c), max(source_ticker, c))
        if cache_key in _CORR_CACHE:
            results[c] = _CORR_CACHE[cache_key]
        else:
            to_fetch.append(c)

    if not to_fetch:
        return results

    try:
        import yfinance as yf
        import pandas as pd

        fetch_tickers = [source_ticker] + to_fetch
        data = yf.download(fetch_tickers, period=period, auto_adjust=True, progress=False)
        if isinstance(data.columns, pd.MultiIndex):
            close = data["Close"]
        else:
            close = data

        close = close.ffill().dropna()
        if len(close) < 30 or source_ticker not in close.columns:
            return results

        returns = close.pct_change().dropna()

        for c in to_fetch:
            if c in returns.columns:
                corr = float(returns[source_ticker].corr(returns[c]))
                cache_key = (min(source_ticker, c), max(source_ticker, c))
                _CORR_CACHE[cache_key] = corr
                results[c] = corr
            else:
                # Don't cache failures — ticker might become available later
                results[c] = 0.0
    except Exception as e:
        import warnings
        warnings.warn(f"Correlation fetch failed: {e}", stacklevel=2)

    return results


def _find_best_substitute(
    ticker: str,
    exclude: set[str],
    min_correlation: float = 0.5,
) -> tuple[Optional[str], Optional[str], float]:
    """Find the best substitute by correlation (batch download).

    Returns (substitute_ticker, industry, correlation) or (None, None, 0).
    """
    candidates = _get_candidates(ticker, exclude)
    if not candidates:
        return None, None, 0.0

    # Batch compute correlations (single yfinance download)
    candidate_tickers = [c[0] for c in candidates]
    correlations = _compute_correlations_batch(ticker, candidate_tickers)

    # Score: (candidate, industry, correlation)
    scored = []
    for candidate, industry in candidates:
        corr = correlations.get(candidate, 0.0)
        scored.append((candidate, industry, corr))

    # Sort by correlation descending
    scored.sort(key=lambda x: x[2], reverse=True)

    # Return best that meets threshold, or best overall with warning
    for candidate, industry, corr in scored:
        if corr >= min_correlation:
            return candidate, industry, corr

    # No candidate meets threshold — return best anyway (caller will warn)
    if scored and scored[0][2] > 0:
        return scored[0]
    return None, None, 0.0


# ---------------------------------------------------------------------------
# Harvest result
# ---------------------------------------------------------------------------


@dataclass
class HarvestTrade:
    """A single harvest trade (sell loss + buy substitute)."""
    sell_ticker: str
    sell_shares: float
    sell_price: float
    cost_basis: float
    loss_pct: float
    realized_loss: float
    tax_benefit: float
    buy_ticker: Optional[str]
    buy_shares: float
    buy_price: float
    industry: Optional[str]
    correlation: float = 0.0


@dataclass
class HarvestResult:
    """Result of tax-loss harvesting analysis."""
    trades: List[HarvestTrade]
    total_loss_harvested: float
    total_tax_benefit: float
    prices: Dict[str, float] = field(default_factory=dict)
    warnings: List[str] = field(default_factory=list)

    def summary(self, print_output: bool = True) -> str:
        lines = [
            "=== taxfolio Tax-Loss Harvest ===",
            f"Positions harvested : {len(self.trades)}",
            f"Total loss harvested: ${self.total_loss_harvested:,.2f}",
            f"Est. tax benefit    : ${self.total_tax_benefit:,.2f}",
            "",
        ]
        for t in self.trades:
            lines.append(
                f"  SELL {t.sell_ticker:>6s}  {t.sell_shares:>8.0f} sh @ ${t.sell_price:>8.2f}  "
                f"({t.loss_pct:+.1f}%)  loss=${t.realized_loss:>10,.2f}  "
                f"tax benefit=${t.tax_benefit:>8,.2f}"
            )
            if t.buy_ticker:
                lines.append(
                    f"   BUY {t.buy_ticker:>6s}  {t.buy_shares:>8.0f} sh @ ${t.buy_price:>8.2f}  "
                    f"r={t.correlation:.2f}  ({t.industry or '?'})"
                )
            else:
                lines.append("   → proceeds to cash (no correlated substitute found)")
            lines.append("")

        if self.warnings:
            lines.append("Warnings:")
            for w in self.warnings:
                lines.append(f"  ! {w}")

        text = "\n".join(lines)
        if print_output:
            print(text)
        return text


# ---------------------------------------------------------------------------
# Main harvest function
# ---------------------------------------------------------------------------


def harvest_losses(
    holdings: Sequence[Holding],
    cash: float = 0,
    *,
    prices: Optional[Dict[str, float]] = None,
    min_loss_pct: float = 5.0,
    min_correlation: float = 0.5,
    max_positions: int = 10,
    tax_rate: float = 0.37,
    wash_sale_days: int = 30,
    recent_sells: Optional[Dict[str, str]] = None,
) -> HarvestResult:
    """Identify tax-loss harvesting opportunities.

    Parameters
    ----------
    holdings : sequence of Holding
        Current positions.
    cash : float
        Available cash.
    prices : dict, optional
        Current prices. If None, fetched via yfinance.
    min_loss_pct : float
        Minimum unrealized loss percentage (e.g., 5.0 = positions down 5%+).
    min_correlation : float
        Minimum return correlation for substitute securities (0-1).
    max_positions : int
        Maximum number of positions to harvest.
    tax_rate : float
        Tax rate for benefit calculation.
    wash_sale_days : int
        Wash sale window in days. US=30, Japan=0, UK=30, etc.
    recent_sells : dict, optional
        {ticker: "YYYY-MM-DD"} of recent sales for wash-sale detection.
    """
    # Fetch prices if needed
    if prices is None:
        try:
            import yfinance as yf
            tickers = list({h.ticker for h in holdings})
            data = yf.download(tickers, period="5d", auto_adjust=True, progress=False)
            import pandas as pd
            if isinstance(data.columns, pd.MultiIndex):
                close = data["Close"]
            else:
                close = data
            close = close.ffill().dropna()
            if len(close) > 0:
                latest = close.iloc[-1]
                prices = {t: float(latest[t]) for t in tickers
                          if t in latest.index and not pd.isna(latest[t])}
            else:
                prices = {}
        except Exception as e:
            raise RuntimeError(
                f"Failed to fetch prices from yfinance: {e}. "
                "Pass prices= explicitly to avoid network calls."
            ) from e

    if not prices:
        return HarvestResult(trades=[], total_loss_harvested=0, total_tax_benefit=0,
                             warnings=["No price data available"])

    # Aggregate holdings per ticker
    agg: Dict[str, dict] = {}
    for h in holdings:
        if h.ticker not in agg:
            agg[h.ticker] = {"shares": 0, "total_cost": 0, "acquired": h.acquired}
        agg[h.ticker]["shares"] += h.shares
        agg[h.ticker]["total_cost"] += h.shares * h.cost_basis

    # Score candidates by unrealized loss percentage
    candidates = []
    for ticker, info in agg.items():
        if ticker not in prices:
            continue
        current_price = prices[ticker]
        avg_cost = info["total_cost"] / info["shares"]
        loss_pct = (current_price / avg_cost - 1) * 100 if avg_cost > 0 else 0
        unrealized_pnl = (current_price - avg_cost) * info["shares"]

        if loss_pct < -min_loss_pct:
            candidates.append({
                "ticker": ticker,
                "shares": info["shares"],
                "avg_cost": avg_cost,
                "current_price": current_price,
                "loss_pct": loss_pct,
                "unrealized_loss": unrealized_pnl,
                "tax_benefit": abs(unrealized_pnl) * tax_rate,
            })

    # Sort by largest loss first
    candidates.sort(key=lambda c: c["unrealized_loss"])
    selected = candidates[:max_positions]

    # Find substitutes and build trades
    portfolio_tickers = set(agg.keys())
    trades = []
    harvest_warnings = []

    for c in selected:
        ticker = c["ticker"]
        substitute, industry, correlation = _find_best_substitute(
            ticker, exclude=portfolio_tickers, min_correlation=min_correlation,
        )

        buy_price = 0.0
        buy_shares = 0.0

        # Reject low-correlation substitutes — harvest to cash instead
        if substitute and correlation < min_correlation:
            harvest_warnings.append(
                f"{ticker}: best substitute {substitute} has r={correlation:.2f} "
                f"(below {min_correlation:.2f}), harvesting to cash"
            )
            substitute = None
            industry = None
            correlation = 0.0

        if substitute:
            if substitute in prices:
                buy_price = prices[substitute]
            else:
                try:
                    import yfinance as yf
                    buy_price = float(yf.Ticker(substitute).fast_info.last_price)
                except Exception:
                    buy_price = 0

        sell_proceeds = c["shares"] * c["current_price"]
        if buy_price > 0:
            buy_shares = int(sell_proceeds / buy_price)

        trades.append(HarvestTrade(
            sell_ticker=ticker,
            sell_shares=c["shares"],
            sell_price=c["current_price"],
            cost_basis=c["avg_cost"],
            loss_pct=c["loss_pct"],
            realized_loss=c["unrealized_loss"],
            tax_benefit=c["tax_benefit"],
            buy_ticker=substitute,
            buy_shares=buy_shares,
            buy_price=buy_price,
            industry=industry,
            correlation=correlation,
        ))

        if substitute:
            portfolio_tickers.add(substitute)

        # Wash sale check
        if recent_sells and ticker in recent_sells and wash_sale_days > 0:
            sell_date_str = recent_sells[ticker]
            try:
                sell_date = date.fromisoformat(sell_date_str)
                days_since = (date.today() - sell_date).days
                if days_since < wash_sale_days:
                    harvest_warnings.append(
                        f"{ticker}: sold {days_since} days ago, "
                        f"within {wash_sale_days}-day wash sale window"
                    )
            except ValueError:
                pass

    total_loss = sum(t.realized_loss for t in trades)
    total_benefit = sum(t.tax_benefit for t in trades)

    return HarvestResult(
        trades=trades,
        total_loss_harvested=total_loss,
        total_tax_benefit=total_benefit,
        prices=prices or {},
        warnings=harvest_warnings,
    )
