# Contributing to taxfolio

Thank you for your interest in contributing to taxfolio.

## Development Setup

```bash
# Prerequisites
brew install cmake eigen osqp  # macOS

# Clone and build
git clone https://github.com/ajtgjmdjp/taxfolio.git
cd taxfolio
uv venv && uv pip install pybind11 numpy yfinance pandas pytest ruff

PYBIND11_DIR=$(uv run python -c "import pybind11; print(pybind11.get_cmake_dir())")
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTAXFOLIO_BUILD_PYTHON=ON -DTAXFOLIO_BUILD_TESTS=ON -Dpybind11_DIR="$PYBIND11_DIR"
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

## Running Tests

```bash
# C++ tests
./build/tests/taxfolio_tests

# Python tests
PYTHONPATH=. uv run --no-project pytest tests/ -v

# Lint
uv run --no-project ruff check taxfolio/
```

## Code Style

- Python: [ruff](https://docs.astral.sh/ruff/) with default settings
- C++: C++20, no external style enforcer (keep consistent with existing code)
- Commit messages: imperative mood, concise

## What to Contribute

- Bug fixes
- Additional tax jurisdictions (UK, Germany, etc.)
- Substitute security mappings for new markets
- Risk model improvements
- Documentation and examples
- Test coverage

## What Not to Submit

- UI/frontend code (taxfolio is a library)
- Broker API integrations (out of scope for core)
- Changes that add heavy dependencies

## Pull Requests

1. Fork the repo
2. Create a feature branch
3. Add tests for new functionality
4. Ensure all tests pass
5. Submit a PR with a clear description

## License

By contributing, you agree that your contributions will be licensed under the AGPL-3.0.
