"""Broker CSV importers — parse positions from various broker export formats."""

from __future__ import annotations

import csv
import io
from pathlib import Path
from typing import List, Optional

from .holding import Holding
from .portfolio import Portfolio


def from_schwab(path: str, cash: float = 0) -> Portfolio:
    """Import positions from Charles Schwab CSV export.

    Expected columns: Symbol, Quantity, Cost/Share or Cost Basis Per Share,
    Date Acquired (optional).

    Schwab CSVs often have header rows and a summary section at the bottom.
    This parser skips non-data rows automatically.
    """
    return _parse_broker_csv(
        path, cash,
        ticker_candidates=["Symbol", "symbol"],
        shares_candidates=["Quantity", "quantity", "Qty"],
        cost_candidates=["Cost/Share", "Cost Basis Per Share", "Cost Per Share",
                         "Unit Cost", "Price Paid"],
        date_candidates=["Date Acquired", "Acquisition Date", "Open Date"],
        skip_non_equity=True,
    )


def from_fidelity(path: str, cash: float = 0) -> Portfolio:
    """Import positions from Fidelity CSV export.

    Expected columns: Symbol, Quantity, Cost Basis Per Share, Date Acquired.
    """
    return _parse_broker_csv(
        path, cash,
        ticker_candidates=["Symbol", "symbol"],
        shares_candidates=["Quantity", "quantity", "Shares"],
        cost_candidates=["Cost Basis Per Share", "Average Cost Basis",
                         "Cost Basis/Share", "Unit Cost"],
        date_candidates=["Date Acquired", "Acquisition Date", "Date Purchased"],
        skip_non_equity=True,
    )


def from_ibkr(path: str, cash: float = 0) -> Portfolio:
    """Import positions from Interactive Brokers CSV export.

    IBKR Activity Statements are multi-section CSVs. This parser
    looks for the "Open Positions" or "Positions" section.
    """
    path_obj = Path(path)
    text = path_obj.read_text(encoding="utf-8-sig")

    # IBKR CSVs have sections like "Trades,Header,..." and "Positions,Header,..."
    # Find the positions section
    lines = text.strip().split("\n")
    position_lines = []
    in_positions = False
    headers = None

    for line in lines:
        parts = line.split(",")
        if len(parts) >= 2:
            section = parts[0].strip().strip('"')
            row_type = parts[1].strip().strip('"')

            if section in ("Open Positions", "Positions", "Position"):
                if row_type == "Header":
                    headers = [p.strip().strip('"') for p in parts[2:]]
                    in_positions = True
                    continue
                elif row_type == "Data" and in_positions and headers:
                    values = [p.strip().strip('"') for p in parts[2:]]
                    position_lines.append(dict(zip(headers, values)))
                elif row_type in ("Total", "SubTotal"):
                    continue
            elif in_positions and section not in ("", "Open Positions", "Positions"):
                in_positions = False

    if not position_lines:
        # Fallback: try as regular CSV
        return _parse_broker_csv(
            path, cash,
            ticker_candidates=["Symbol", "symbol"],
            shares_candidates=["Quantity", "Position", "Shares"],
            cost_candidates=["Cost Basis Per Share", "Cost Price", "Avg Cost"],
            date_candidates=["Date Acquired", "Open Date"],
        )

    holdings: List[Holding] = []
    for i, row in enumerate(position_lines, start=1):
        ticker = _find_value(row, ["Symbol", "Financial Instrument"])
        shares_str = _find_value(row, ["Quantity", "Position", "Shares"])
        cost_str = _find_value(row, ["Cost Basis Per Share", "Cost Price",
                                      "Avg Cost", "Cost Basis"])
        date_str = _find_value(row, ["Date Acquired", "Open Date"])

        if not ticker or not shares_str:
            continue

        try:
            shares = float(shares_str.replace(",", ""))
            cost = float(cost_str.replace(",", "").replace("$", "")) if cost_str else 0.0
        except ValueError:
            continue

        if shares <= 0:
            continue

        holdings.append(Holding(
            ticker=ticker,
            shares=shares,
            cost_basis=cost,
            acquired=date_str or "2024-01-01",
            lot_id=i,
        ))

    return Portfolio(holdings=holdings, cash=cash)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _find_value(row: dict, candidates: list[str]) -> Optional[str]:
    """Find a value in a dict by trying multiple key names."""
    for key in candidates:
        if key in row and row[key]:
            return row[key].strip()
    return None


def _parse_broker_csv(
    path: str,
    cash: float,
    ticker_candidates: list[str],
    shares_candidates: list[str],
    cost_candidates: list[str],
    date_candidates: list[str],
    skip_non_equity: bool = False,
) -> Portfolio:
    """Generic broker CSV parser with flexible column matching."""
    path_obj = Path(path)
    text = path_obj.read_text(encoding="utf-8-sig")

    # Skip common header/footer lines
    lines = text.strip().split("\n")
    clean_lines = []
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("Account") or stripped.startswith("Note"):
            continue
        clean_lines.append(stripped)

    reader = csv.DictReader(io.StringIO("\n".join(clean_lines)))
    if not reader.fieldnames:
        raise ValueError(f"No headers found in {path}")

    headers = list(reader.fieldnames)

    def find_col(candidates: list[str]) -> Optional[str]:
        header_lower = {h.lower().strip(): h for h in headers}
        for c in candidates:
            if c.lower() in header_lower:
                return header_lower[c.lower()]
        return None

    tc = find_col(ticker_candidates)
    sc = find_col(shares_candidates)
    cc = find_col(cost_candidates)
    dc = find_col(date_candidates)

    if not tc:
        raise ValueError(f"No ticker column found. Headers: {headers}")
    if not sc:
        raise ValueError(f"No shares column found. Headers: {headers}")

    holdings: List[Holding] = []
    for i, row in enumerate(reader, start=1):
        ticker = row.get(tc, "").strip()
        shares_str = row.get(sc, "").strip()

        if not ticker or not shares_str:
            continue

        # Skip non-equity rows (cash, options, etc.)
        if skip_non_equity:
            if any(skip in ticker.upper() for skip in
                   ["CASH", "MONEY MARKET", "SWEEP", "--", "**"]):
                continue

        try:
            shares = float(shares_str.replace(",", ""))
        except ValueError:
            continue

        if shares <= 0:
            continue

        cost_str = row.get(cc, "").strip() if cc else ""
        try:
            cost = float(cost_str.replace(",", "").replace("$", "")) if cost_str else 0.0
        except ValueError:
            cost = 0.0

        date_str = row.get(dc, "").strip() if dc else ""

        holdings.append(Holding(
            ticker=ticker,
            shares=shares,
            cost_basis=cost,
            acquired=date_str or "2024-01-01",
            lot_id=i,
        ))

    return Portfolio(holdings=holdings, cash=cash)
