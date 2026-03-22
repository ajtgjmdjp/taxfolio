#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/chrono.h>
#include <pybind11/numpy.h>
#include "api.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace py = pybind11;

// Helper: parse ISO date string "YYYY-MM-DD" to sys_days
static ov::Date parse_date(const std::string& s) {
    int y, m, d;
    char sep1, sep2;
    std::istringstream iss(s);
    iss >> y >> sep1 >> m >> sep2 >> d;
    return std::chrono::sys_days{
        std::chrono::year{y} / std::chrono::month{static_cast<unsigned>(m)}
        / std::chrono::day{static_cast<unsigned>(d)}
    };
}

// Helper: format sys_days to "YYYY-MM-DD"
static std::string format_date(ov::Date d) {
    auto ymd = std::chrono::year_month_day{d};
    std::ostringstream oss;
    oss << static_cast<int>(ymd.year()) << '-'
        << std::setfill('0') << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-'
        << std::setfill('0') << std::setw(2) << static_cast<unsigned>(ymd.day());
    return oss.str();
}

// Helper: numpy array to DenseMatrix
static ov::DenseMatrix numpy_to_dense(py::array_t<double> arr) {
    auto buf = arr.request();
    ov::DenseMatrix m;
    if (buf.ndim == 2) {
        m.rows = buf.shape[0];
        m.cols = buf.shape[1];
    } else if (buf.ndim == 1) {
        m.rows = buf.shape[0];
        m.cols = 1;
    } else {
        throw std::runtime_error("Expected 1D or 2D numpy array");
    }
    m.values.resize(m.rows * m.cols);
    auto ptr = static_cast<double*>(buf.ptr);
    std::copy(ptr, ptr + m.rows * m.cols, m.values.begin());
    return m;
}

// Helper: numpy array to vector<double>
static std::vector<double> numpy_to_vec(py::array_t<double> arr) {
    auto buf = arr.request();
    auto ptr = static_cast<double*>(buf.ptr);
    return std::vector<double>(ptr, ptr + buf.size);
}

PYBIND11_MODULE(_taxfolio, m) {
    m.doc() = "OpenVolt: Professional-grade portfolio optimization engine";

    // --- Enums ---
    py::enum_<ov::Side>(m, "Side")
        .value("buy", ov::Side::buy)
        .value("sell", ov::Side::sell);

    py::enum_<ov::DisposalMethod>(m, "DisposalMethod")
        .value("specific_id", ov::DisposalMethod::specific_id)
        .value("fifo", ov::DisposalMethod::fifo)
        .value("lifo", ov::DisposalMethod::lifo);

    py::enum_<ov::TaxCharacter>(m, "TaxCharacter")
        .value("short_term", ov::TaxCharacter::short_term)
        .value("long_term", ov::TaxCharacter::long_term);

    // --- DenseMatrix ---
    py::class_<ov::DenseMatrix>(m, "DenseMatrix")
        .def(py::init<>())
        .def_readwrite("rows", &ov::DenseMatrix::rows)
        .def_readwrite("cols", &ov::DenseMatrix::cols)
        .def_readwrite("values", &ov::DenseMatrix::values);

    // --- TaxLot ---
    py::class_<ov::TaxLot>(m, "TaxLot")
        .def(py::init([](uint64_t lot_id, std::string asset_id, double shares,
                         double cost_basis_per_share, std::string acquired_on) {
            return ov::TaxLot{
                lot_id, std::move(asset_id), shares,
                cost_basis_per_share, parse_date(acquired_on)
            };
        }), py::arg("lot_id"), py::arg("asset_id"), py::arg("shares"),
            py::arg("cost_basis_per_share"), py::arg("acquired_on"))
        .def_readwrite("lot_id", &ov::TaxLot::lot_id)
        .def_readwrite("asset_id", &ov::TaxLot::asset_id)
        .def_readwrite("shares", &ov::TaxLot::shares)
        .def_readwrite("cost_basis_per_share", &ov::TaxLot::cost_basis_per_share)
        .def_property("acquired_on",
            [](const ov::TaxLot& t) { return format_date(t.acquired_on); },
            [](ov::TaxLot& t, const std::string& s) { t.acquired_on = parse_date(s); });

    // --- PortfolioState ---
    py::class_<ov::PortfolioState>(m, "PortfolioState")
        .def(py::init([](std::string as_of, double cash, std::vector<ov::TaxLot> lots) {
            return ov::PortfolioState{parse_date(as_of), cash, std::move(lots)};
        }), py::arg("as_of"), py::arg("cash"), py::arg("lots"))
        .def_readwrite("cash", &ov::PortfolioState::cash)
        .def_readwrite("lots", &ov::PortfolioState::lots)
        .def_property("as_of",
            [](const ov::PortfolioState& p) { return format_date(p.as_of); },
            [](ov::PortfolioState& p, const std::string& s) { p.as_of = parse_date(s); });

    // --- FullCovarianceRisk ---
    py::class_<ov::FullCovarianceRisk>(m, "FullCovarianceRisk")
        .def(py::init([](std::vector<std::string> asset_ids, py::array_t<double> cov) {
            ov::FullCovarianceRisk r;
            r.asset_ids = std::move(asset_ids);
            r.covariance = numpy_to_dense(cov);
            return r;
        }), py::arg("asset_ids"), py::arg("covariance"))
        .def_readwrite("asset_ids", &ov::FullCovarianceRisk::asset_ids)
        .def_readwrite("covariance", &ov::FullCovarianceRisk::covariance);

    // --- FactorRisk ---
    py::class_<ov::FactorRisk>(m, "FactorRisk")
        .def(py::init([](std::vector<std::string> asset_ids,
                         std::vector<std::string> factor_ids,
                         py::array_t<double> exposures,
                         py::array_t<double> factor_covariance,
                         py::array_t<double> specific_variance) {
            ov::FactorRisk r;
            r.asset_ids = std::move(asset_ids);
            r.factor_ids = std::move(factor_ids);
            r.exposures = numpy_to_dense(exposures);
            r.factor_covariance = numpy_to_dense(factor_covariance);
            r.specific_variance = numpy_to_vec(specific_variance);
            return r;
        }), py::arg("asset_ids"), py::arg("factor_ids"),
            py::arg("exposures"), py::arg("factor_covariance"),
            py::arg("specific_variance"))
        .def_readwrite("asset_ids", &ov::FactorRisk::asset_ids)
        .def_readwrite("factor_ids", &ov::FactorRisk::factor_ids);

    // --- WeightBound ---
    py::class_<ov::WeightBound>(m, "WeightBound")
        .def(py::init<double, double>(),
             py::arg("min_weight") = 0.0, py::arg("max_weight") = 1.0)
        .def_readwrite("min_weight", &ov::WeightBound::min_weight)
        .def_readwrite("max_weight", &ov::WeightBound::max_weight);

    // --- Constraints ---
    py::class_<ov::Constraints>(m, "Constraints")
        .def(py::init<>())
        .def_readwrite("max_turnover", &ov::Constraints::max_turnover)
        .def_readwrite("cash_buffer", &ov::Constraints::cash_buffer)
        .def_readwrite("weight_bounds", &ov::Constraints::weight_bounds)
        .def_readwrite("no_buy", &ov::Constraints::no_buy)
        .def_readwrite("no_sell", &ov::Constraints::no_sell);

    // --- TaxConfig ---
    py::class_<ov::TaxConfig>(m, "TaxConfig")
        .def(py::init<>())
        .def_readwrite("disposal_method", &ov::TaxConfig::disposal_method)
        .def_readwrite("short_term_rate", &ov::TaxConfig::short_term_rate)
        .def_readwrite("long_term_rate", &ov::TaxConfig::long_term_rate)
        .def_readwrite("allow_loss_harvesting", &ov::TaxConfig::allow_loss_harvesting)
        .def_readwrite("wash_sale_window_days", &ov::TaxConfig::wash_sale_window_days);

    // --- ObjectiveWeights ---
    py::class_<ov::ObjectiveWeights>(m, "ObjectiveWeights")
        .def(py::init<>())
        .def_readwrite("allocation_drift", &ov::ObjectiveWeights::allocation_drift)
        .def_readwrite("transaction_cost", &ov::ObjectiveWeights::transaction_cost)
        .def_readwrite("tax_cost", &ov::ObjectiveWeights::tax_cost)
        .def_readwrite("alpha_weight", &ov::ObjectiveWeights::alpha_weight)
        .def_readwrite("alpha_vector", &ov::ObjectiveWeights::alpha_vector);

    // --- OptimizationConfig ---
    py::class_<ov::OptimizationConfig>(m, "OptimizationConfig")
        .def(py::init<>())
        .def_readwrite("constraints", &ov::OptimizationConfig::constraints)
        .def_readwrite("taxes", &ov::OptimizationConfig::taxes)
        .def_readwrite("objective", &ov::OptimizationConfig::objective)
        .def_readwrite("min_trade_notional", &ov::OptimizationConfig::min_trade_notional)
        .def_readwrite("round_to_whole_shares", &ov::OptimizationConfig::round_to_whole_shares)
        .def_readwrite("solver", &ov::OptimizationConfig::solver);

    // --- MarketData ---
    py::class_<ov::MarketData>(m, "MarketData")
        .def(py::init([](std::string as_of,
                         std::vector<std::string> asset_ids,
                         py::array_t<double> prices,
                         py::array_t<double> benchmark_weights,
                         py::array_t<double> transaction_cost_bps,
                         py::object risk_model) {
            ov::MarketData md;
            md.as_of = parse_date(as_of);
            md.asset_ids = std::move(asset_ids);
            md.prices = numpy_to_vec(prices);
            md.benchmark_weights = numpy_to_vec(benchmark_weights);
            md.transaction_cost_bps = numpy_to_vec(transaction_cost_bps);
            if (py::isinstance<ov::FullCovarianceRisk>(risk_model)) {
                md.risk_model = risk_model.cast<ov::FullCovarianceRisk>();
            } else if (py::isinstance<ov::FactorRisk>(risk_model)) {
                md.risk_model = risk_model.cast<ov::FactorRisk>();
            } else {
                throw std::runtime_error("risk_model must be FullCovarianceRisk or FactorRisk");
            }
            return md;
        }), py::arg("as_of"), py::arg("asset_ids"), py::arg("prices"),
            py::arg("benchmark_weights"), py::arg("transaction_cost_bps"),
            py::arg("risk_model"))
        .def_readwrite("asset_ids", &ov::MarketData::asset_ids)
        .def_readwrite("prices", &ov::MarketData::prices)
        .def_readwrite("benchmark_weights", &ov::MarketData::benchmark_weights);

    // --- RebalanceRequest ---
    py::class_<ov::RebalanceRequest>(m, "RebalanceRequest")
        .def(py::init<ov::PortfolioState, ov::MarketData, ov::OptimizationConfig>(),
             py::arg("portfolio"), py::arg("market"), py::arg("config"))
        .def_readwrite("portfolio", &ov::RebalanceRequest::portfolio)
        .def_readwrite("market", &ov::RebalanceRequest::market)
        .def_readwrite("config", &ov::RebalanceRequest::config);

    // --- Trade ---
    py::class_<ov::Trade>(m, "Trade")
        .def_readonly("asset_id", &ov::Trade::asset_id)
        .def_readonly("side", &ov::Trade::side)
        .def_readonly("shares", &ov::Trade::shares)
        .def_readonly("notional", &ov::Trade::notional)
        .def("__repr__", [](const ov::Trade& t) {
            return "<Trade " + t.asset_id + " " +
                   (t.side == ov::Side::buy ? "BUY" : "SELL") +
                   " " + std::to_string(t.shares) + " shares>";
        });

    // --- LotDisposition ---
    py::class_<ov::LotDisposition>(m, "LotDisposition")
        .def_readonly("lot_id", &ov::LotDisposition::lot_id)
        .def_readonly("asset_id", &ov::LotDisposition::asset_id)
        .def_readonly("shares_sold", &ov::LotDisposition::shares_sold)
        .def_readonly("proceeds", &ov::LotDisposition::proceeds)
        .def_readonly("cost_basis", &ov::LotDisposition::cost_basis)
        .def_readonly("realized_gain", &ov::LotDisposition::realized_gain)
        .def_readonly("tax_character", &ov::LotDisposition::tax_character)
        .def_readonly("tax_liability", &ov::LotDisposition::tax_liability);

    // --- Diagnostics ---
    py::class_<ov::Diagnostics>(m, "Diagnostics")
        .def_readonly("converged", &ov::Diagnostics::converged)
        .def_readonly("solver_status", &ov::Diagnostics::solver_status)
        .def_readonly("objective_value", &ov::Diagnostics::objective_value)
        .def_readonly("ex_ante_allocation_drift", &ov::Diagnostics::ex_ante_allocation_drift)
        .def_readonly("turnover", &ov::Diagnostics::turnover)
        .def_readonly("estimated_transaction_cost", &ov::Diagnostics::estimated_transaction_cost)
        .def_readonly("estimated_tax_cost", &ov::Diagnostics::estimated_tax_cost);

    // --- RebalanceResult ---
    py::class_<ov::RebalanceResult>(m, "RebalanceResult")
        .def_readonly("trades", &ov::RebalanceResult::trades)
        .def_readonly("lot_dispositions", &ov::RebalanceResult::lot_dispositions)
        .def_readonly("target_weights", &ov::RebalanceResult::target_weights)
        .def_readonly("diagnostics", &ov::RebalanceResult::diagnostics);

    // --- Core function ---
    m.def("plan_rebalance", &ov::plan_rebalance,
          py::arg("request"),
          py::call_guard<py::gil_scoped_release>(),
          "Plan a portfolio rebalance. Returns optimal trades with tax lot dispositions.");
}
