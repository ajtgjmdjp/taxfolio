"""Tests for tax-loss harvesting."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "build"))

from taxfolio import Portfolio, Holding, HarvestResult


class TestSubstituteMap:
    def test_get_candidates(self):
        from taxfolio.harvest import _get_candidates
        candidates = _get_candidates("INTC", exclude={"NVDA"})
        assert len(candidates) > 0
        tickers = [c[0] for c in candidates]
        assert "INTC" not in tickers
        assert "NVDA" not in tickers

    def test_find_best_substitute(self):
        from taxfolio.harvest import _find_best_substitute
        sub, industry, corr = _find_best_substitute("INTC", exclude=set(), min_correlation=0.0)
        assert sub is not None
        assert industry is not None
        assert isinstance(corr, float)

    def test_find_industry_via_map(self):
        from taxfolio.harvest import _get_candidates
        candidates = _get_candidates("BA", exclude=set())
        industries = {c[1] for c in candidates}
        assert "Aerospace & Defense" in industries


class TestHarvestLosses:
    def test_with_prices(self):
        portfolio = Portfolio(
            holdings=[
                Holding("INTC", 200, 45.0, "2024-06-01"),
                Holding("NVDA", 50, 130.0, "2024-06-01"),
            ],
            cash=5000,
        )
        result = portfolio.harvest_losses(
            prices={"INTC": 22.0, "NVDA": 170.0},
            min_loss_pct=5.0,
            min_correlation=0.0,  # accept any for unit test speed
        )
        assert isinstance(result, HarvestResult)
        assert len(result.trades) >= 1
        assert result.trades[0].sell_ticker == "INTC"
        assert result.trades[0].loss_pct < -5.0
        assert result.total_tax_benefit > 0
        assert "INTC" in result.prices

    def test_no_losses(self):
        portfolio = Portfolio(
            holdings=[Holding("AAPL", 100, 100.0, "2024-01-01")],
            cash=5000,
        )
        result = portfolio.harvest_losses(
            prices={"AAPL": 200.0},
            min_loss_pct=5.0,
        )
        assert len(result.trades) == 0

    def test_min_loss_threshold(self):
        portfolio = Portfolio(
            holdings=[Holding("INTC", 10, 45.0, "2024-06-01")],
            cash=0,
        )
        result = portfolio.harvest_losses(
            prices={"INTC": 44.0},  # -2.2%, below 5% threshold
            min_loss_pct=5.0,
        )
        assert len(result.trades) == 0

    def test_max_positions(self):
        portfolio = Portfolio(
            holdings=[
                Holding("INTC", 200, 45.0, "2024-06-01"),
                Holding("NKE", 100, 110.0, "2024-03-01"),
                Holding("PFE", 300, 35.0, "2024-06-15"),
            ],
            cash=0,
        )
        result = portfolio.harvest_losses(
            prices={"INTC": 22.0, "NKE": 70.0, "PFE": 25.0},
            min_loss_pct=1.0,
            min_correlation=0.0,
            max_positions=2,
        )
        assert len(result.trades) <= 2

    def test_wash_sale_warning(self):
        portfolio = Portfolio(
            holdings=[Holding("INTC", 200, 45.0, "2024-06-01")],
            cash=0,
        )
        from datetime import date, timedelta
        recent_date = (date.today() - timedelta(days=10)).isoformat()
        result = portfolio.harvest_losses(
            prices={"INTC": 22.0},
            min_loss_pct=5.0,
            min_correlation=0.0,
            wash_sale_days=30,
            recent_sells={"INTC": recent_date},
        )
        assert any("wash sale" in w for w in result.warnings)

    def test_wash_sale_japan_no_rule(self):
        portfolio = Portfolio(
            holdings=[Holding("INTC", 200, 45.0, "2024-06-01")],
            cash=0,
        )
        from datetime import date, timedelta
        recent_date = (date.today() - timedelta(days=10)).isoformat()
        result = portfolio.harvest_losses(
            prices={"INTC": 22.0},
            min_loss_pct=5.0,
            min_correlation=0.0,
            wash_sale_days=0,  # Japan: no wash sale rule
            recent_sells={"INTC": recent_date},
        )
        assert not any("wash sale" in w for w in result.warnings)

    def test_correlation_in_result(self):
        portfolio = Portfolio(
            holdings=[Holding("INTC", 200, 45.0, "2024-06-01")],
            cash=0,
        )
        result = portfolio.harvest_losses(
            prices={"INTC": 22.0},
            min_loss_pct=5.0,
            min_correlation=0.0,
        )
        if result.trades and result.trades[0].buy_ticker:
            assert isinstance(result.trades[0].correlation, float)
