#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <Eigen/Dense>

namespace openvolt {

// Time representation
using Date = std::chrono::year_month_day;
using Timestamp = std::chrono::system_clock::time_point;

// Ticker identifier
using Ticker = std::string;

// Weight and return vectors (Eigen for performance)
using Vector = Eigen::VectorXd;
using Matrix = Eigen::MatrixXd;

// Common type aliases
using Weight = double;
using Price = double;
using Shares = double;
using Money = double;

} // namespace openvolt
