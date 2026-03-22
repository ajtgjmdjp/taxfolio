# Changelog

## [0.2.0] - 2026-03-22

### Added
- `pip install .` support via scikit-build-core (no manual cmake required)
- Broker CSV importers: `from_schwab()`, `from_fidelity()`, `from_ibkr()`
- Correlation-based substitute matching for TLH (`min_correlation` parameter)
- `wash_sale_days` parameter (US=30, Japan=0, configurable)
- `min_loss_pct` threshold (percentage-based, replaces absolute)
- Low-correlation substitutes rejected → harvest to cash with warning
- Explicit warnings for: missing price data, covariance fallbacks, correlation fetch failures
- `HarvestResult.prices` for reusing fetched prices in optimize()

### Changed
- Default `max_turnover` from 1.0 to 0.15
- Default `max_positions` from 5 to 10
- Substitute map expanded to 220 tickers across 42 industries

### Known Limitations
- Source install requires cmake + eigen + osqp (no prebuilt wheels yet)
- Substitute matching is heuristic (industry + correlation)
- Market data via yfinance (may be delayed or unavailable)
- US and Japan tax jurisdictions only

## [0.1.0a0] - 2026-03-22

### Added
- Initial release
- `Portfolio.optimize()` — C++20 QP solver with buy/sell split
- `Portfolio.harvest_losses()` — TLH with industry-based substitutes
- `Portfolio.from_csv()` — flexible CSV import with column aliases
- 3 risk models: sample, EWMA, Ledoit-Wolf shrinkage
- US (37%/20% ST/LT) and Japan (20.315% flat) tax rules
- Boyd-lite tax model (lot-level ST/LT rates)
