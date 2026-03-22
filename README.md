# taxfolio

**Tax-aware portfolio optimization for Python, backed by C++20.**

The only open-source library that does tax-loss harvesting with automatic substitute security matching.

```python
import taxfolio as tf

portfolio = tf.Portfolio(
    holdings=[
        tf.Holding("INTC", 200, 45.0, "2024-06-01"),   # bought $45, now ~$22
        tf.Holding("NVDA", 50, 130.0, "2024-06-01"),    # winner
        tf.Holding("PFE", 300, 35.0, "2024-06-15"),     # bought $35, now ~$25
    ],
    cash=5000,
)

harvest = portfolio.harvest_losses()
harvest.summary()
```

```
=== taxfolio Tax-Loss Harvest ===
Positions harvested : 2
Total loss harvested: $-7,009.00
Est. tax benefit    : $2,593.33

  SELL   INTC       200 sh @ $   22.00  loss=$ -4,600.00  tax benefit=$1,702.00
   BUY    AMD       100 sh @ $   100.50  (Semiconductors)

  SELL    PFE       300 sh @ $   27.00  loss=$ -2,409.00  tax benefit=$  891.33
   BUY    JNJ        34 sh @ $   235.00  (Drug Manufacturers - General)
```

Sell losers. Buy same-industry substitutes. Avoid wash sales. Automatically.

## Why taxfolio

Existing portfolio optimization libraries ignore taxes. In practice, taxes are the largest controllable cost for taxable accounts — often 1-2% annually.

| Library | Tax-Loss Harvesting | Tax Lots | Substitute Matching | C++ Core |
|---------|:---:|:---:|:---:|:---:|
| PyPortfolioOpt | No | No | No | No |
| Riskfolio-Lib | No | No | No | No |
| skfolio | No | No | No | No |
| cvxportfolio | No | No | No | No |
| **taxfolio** | **Yes** | **Yes** | **Yes** | **Yes** |

## Features

- **Tax-loss harvesting** — detect unrealized losses, find same-industry substitutes, generate sell/buy pairs
- **200+ substitute securities** across 30 industry groups (US market)
- **Tax-lot-aware optimization** — knows which lots to sell (LTFO, FIFO, LIFO, specific ID)
- **ST/LT rate distinction** — short-term vs long-term capital gains rates
- **Buy/sell split QP** — asymmetric: tax penalty applies only to sells
- **3 risk models** — sample covariance, EWMA, Ledoit-Wolf shrinkage (from live yfinance data)
- **C++20 + OSQP** — sub-millisecond QP solves
- **US + Japan tax rules** — 37%/20% ST/LT (US), 20.315% flat (Japan)

## Install

```bash
# Prerequisites (system libraries)
brew install cmake eigen osqp  # macOS
# sudo apt-get install cmake libeigen3-dev  # Ubuntu + build OSQP from source

# Install
git clone https://github.com/ajtgjmdjp/taxfolio.git
cd taxfolio
uv venv && uv pip install .

# Try it
uv run --no-project python examples/quickstart.py
```

## Usage

### Tax-Loss Harvesting

```python
import taxfolio as tf

portfolio = tf.Portfolio.from_csv("my_holdings.csv", cash=5000)
harvest = portfolio.harvest_losses(min_loss=500, max_positions=5)
harvest.summary()
```

### Import from Broker

```python
import taxfolio as tf

# Schwab
portfolio = tf.from_schwab("schwab_positions.csv", cash=10000)

# Fidelity
portfolio = tf.from_fidelity("fidelity_positions.csv")

# Interactive Brokers
portfolio = tf.from_ibkr("ibkr_activity.csv")

# Or any CSV with flexible column names
portfolio = tf.Portfolio.from_csv("my_holdings.csv", cash=5000)
```

### Tax-Aware Rebalancing (advanced)

If you have cash to deploy or want to rebalance toward target weights:

```python
result = portfolio.optimize(
    prices=harvest.prices,   # reuse prices from harvest (no extra API call)
    max_turnover=0.15,
)
result.summary()
```

## How It Works

### Tax-Loss Harvesting

1. Fetch current prices via yfinance
2. Identify positions with unrealized losses exceeding threshold
3. For each loss position, find a same-industry substitute from 200+ securities across 30 industry groups
4. Generate sell/buy pairs that maintain sector exposure while harvesting the tax loss

### Portfolio Optimization

Solves a quadratic program with **separate buy and sell variables**:

```
minimize   λ_drift · (w - w_ref)' Σ (w - w_ref)    allocation drift
         + λ_txn · (buy + sell)                      transaction cost
         + λ_tax · tax_penalty' · sell               tax cost (sells only)

subject to  w = w_current + buy - sell
            Σw = 1,  0 ≤ w ≤ cap
            0.5 · Σ(buy + sell) ≤ turnover_limit
```

Tax penalties are computed per-lot with correct ST/LT rates. Based on [Boyd et al. (2021)](https://arxiv.org/abs/2008.04985).

## Tests

```bash
./build/tests/taxfolio_tests           # 25 C++ tests
PYTHONPATH=. uv run --no-project pytest tests/ -v  # 20 Python tests
```

## Limitations

- **Not tax advice.** taxfolio is a research and decision-support tool. Consult a tax professional before executing trades.
- **Market data from Yahoo Finance.** Prices and correlations are fetched via yfinance. Data may be delayed, incomplete, or unavailable.
- **Heuristic substitute matching.** Substitutes are selected by industry + correlation. The IRS "substantially identical" standard is vague — taxfolio does not guarantee wash sale compliance.
- **Wash sale detection requires user input.** You must provide `recent_sells` for wash sale checks. taxfolio does not track your trade history.
- **US and Japan only.** Other tax jurisdictions are not yet supported.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[AGPL-3.0](LICENSE) — Free to use, modify, and distribute. If you run a modified version as a service, you must release the source.
