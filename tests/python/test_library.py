"""Tests for the taxfolio public library API."""

import sys
import tempfile
from pathlib import Path

import pytest

# Ensure taxfolio package and C++ build are importable
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))

from taxfolio import Portfolio, Holding, OptimizationResult


class TestHolding:
    def test_create(self):
        h = Holding("AAPL", 100, 150.0, "2025-01-15")
        assert h.ticker == "AAPL"
        assert h.shares == 100
        assert h.cost_basis == 150.0
        assert h.acquired == "2025-01-15"

    def test_is_japan(self):
        assert Holding("7203.T", 100, 2500.0, "2025-01-01").is_japan is True
        assert Holding("AAPL", 100, 150.0, "2025-01-01").is_japan is False

    def test_frozen(self):
        h = Holding("AAPL", 100, 150.0, "2025-01-01")
        with pytest.raises(AttributeError):
            h.shares = 200  # type: ignore


class TestPortfolioCreation:
    def test_manual(self):
        p = Portfolio(
            holdings=[
                Holding("AAPL", 100, 150.0, "2025-01-15"),
                Holding("NVDA", 50, 480.0, "2025-03-01"),
            ],
            cash=5000,
        )
        assert len(p.holdings) == 2
        assert p.cash == 5000

    def test_from_csv(self):
        csv_content = "ticker,shares,cost_basis_per_share,acquired_on\nAAPL,100,150.0,2025-01-15\nNVDA,50,480.0,2025-03-01\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv_content)
            f.flush()
            p = Portfolio.from_csv(f.name, cash=5000)
        assert len(p.holdings) == 2
        assert p.holdings[0].ticker == "AAPL"
        assert p.holdings[1].cost_basis == 480.0

    def test_from_csv_aliases(self):
        csv_content = "symbol,quantity,avg_cost,purchase_date\nAMD,200,120.5,2025-06-01\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv_content)
            f.flush()
            p = Portfolio.from_csv(f.name)
        assert len(p.holdings) == 1
        assert p.holdings[0].ticker == "AMD"
        assert p.holdings[0].shares == 200

    def test_from_csv_missing_column(self):
        csv_content = "name,amount\nAAPL,100\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv_content)
            f.flush()
            with pytest.raises(ValueError):
                Portfolio.from_csv(f.name)

    def test_empty_portfolio(self):
        p = Portfolio(holdings=[], cash=10000)
        assert len(p.holdings) == 0


class TestPortfolioOptimize:
    """Tests that require the C++ engine to be built."""

    @pytest.fixture
    def small_portfolio(self):
        return Portfolio(
            holdings=[
                Holding("AAPL", 100, 150.0, "2025-01-15"),
                Holding("MSFT", 50, 400.0, "2025-03-01"),
            ],
            cash=5000,
        )

    def test_optimize_returns_result(self, small_portfolio):
        try:
            result = small_portfolio.optimize(
                prices={"AAPL": 200.0, "MSFT": 450.0},
            )
            assert isinstance(result, OptimizationResult)
            assert result.converged is True
        except RuntimeError as e:
            if "not available" in str(e):
                pytest.skip("C++ engine not built")
            raise

    def test_optimize_with_prices(self, small_portfolio):
        try:
            result = small_portfolio.optimize(
                prices={"AAPL": 200.0, "MSFT": 450.0},
                max_turnover=0.5,
            )
            assert result.converged
            assert isinstance(result.turnover, float)
        except RuntimeError as e:
            if "not available" in str(e):
                pytest.skip("C++ engine not built")
            raise

    def test_result_summary(self, small_portfolio):
        try:
            result = small_portfolio.optimize(
                prices={"AAPL": 200.0, "MSFT": 450.0},
            )
            text = result.summary(print_output=False)
            assert "taxfolio" in text
            assert "Solver status" in text
        except RuntimeError as e:
            if "not available" in str(e):
                pytest.skip("C++ engine not built")
            raise

    def test_japan_jurisdiction(self):
        p = Portfolio(
            holdings=[Holding("7203.T", 100, 2500.0, "2025-01-01")],
            cash=100000,
        )
        try:
            result = p.optimize(
                tax_jurisdiction="japan",
                prices={"7203.T": 2800.0},
            )
            assert result.converged
        except RuntimeError as e:
            if "not available" in str(e):
                pytest.skip("C++ engine not built")
            raise
