"""Tests for broker CSV importers."""

import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from taxfolio import from_schwab, from_fidelity, from_ibkr, Portfolio


class TestSchwabImport:
    def test_basic(self):
        csv = "Symbol,Quantity,Cost/Share,Date Acquired\nAAPL,100,150.00,01/15/2024\nNVDA,50,480.00,03/01/2024\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv)
            f.flush()
            p = from_schwab(f.name, cash=5000)
        assert len(p.holdings) == 2
        assert p.holdings[0].ticker == "AAPL"
        assert p.holdings[0].shares == 100
        assert p.holdings[0].cost_basis == 150.0
        assert p.cash == 5000

    def test_skip_cash_rows(self):
        csv = "Symbol,Quantity,Cost/Share\nAAPL,100,150.00\nCASH,0,0\nSWEEP MONEY,0,0\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv)
            f.flush()
            p = from_schwab(f.name)
        assert len(p.holdings) == 1
        assert p.holdings[0].ticker == "AAPL"


class TestFidelityImport:
    def test_basic(self):
        csv = "Symbol,Shares,Cost Basis Per Share,Date Acquired\nMSFT,40,400.00,2024-06-01\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv)
            f.flush()
            p = from_fidelity(f.name)
        assert len(p.holdings) == 1
        assert p.holdings[0].ticker == "MSFT"
        assert p.holdings[0].shares == 40


class TestIBKRImport:
    def test_multi_section_csv(self):
        csv = """Statement,Header,Field Name,Field Value
Statement,Data,Period,"January 1, 2024 - December 31, 2024"
Open Positions,Header,Symbol,Quantity,Cost Price
Open Positions,Data,AAPL,100,150.00
Open Positions,Data,NVDA,50,480.00
Open Positions,Total,,150,
Trades,Header,Symbol,Date/Time,Quantity,T. Price
Trades,Data,AAPL,2024-06-15,10,200.00
"""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv)
            f.flush()
            p = from_ibkr(f.name)
        assert len(p.holdings) == 2
        assert p.holdings[0].ticker == "AAPL"
        assert p.holdings[1].ticker == "NVDA"

    def test_fallback_to_regular_csv(self):
        csv = "Symbol,Quantity,Cost Price\nAAPL,100,150.00\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as f:
            f.write(csv)
            f.flush()
            p = from_ibkr(f.name)
        assert len(p.holdings) == 1
