#include "core/optimizer/optimizer.hpp"
#include <osqp.h>
#include <Eigen/Sparse>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace openvolt {

// ---------------------------------------------------------------------------
// OSQP-based QP Optimizer (Buy/Sell Split Formulation)
// ---------------------------------------------------------------------------
//
// The portfolio optimization problem:
//
//   minimize   λ_drift * (w - w_ref)' Cov (w - w_ref)    [allocation drift]
//            + c_buy' * b                                  [buy cost]
//            + c_sell' * s                                 [sell cost + tax]
//
//   subject to w - b + s = w_c           (inventory balance)
//              sum(w) = invest_fraction
//              lb <= w <= ub
//              b >= 0,  s >= 0
//              0.5 * sum(b + s) <= turnover_cap
//
// Decision variables: x = [w; b; s] (3N)
//
// c_buy[i]  = λ_tcost + per_asset_tcost[i]
// c_sell[i] = λ_tcost + per_asset_tcost[i] + λ_tax * gain_penalty[i]
//
// gain_penalty[i] = max(0, tax_weighted_gain[i]) — positive only (safe mode)
// Loss harvesting benefit requires anti-roundtrip logic (future work).

class OSQPOptimizer final : public Optimizer {
public:
    [[nodiscard]] OptimizationResult solve(
        const Vector& benchmark_weights,
        const Vector& current_weights,
        const Matrix& cov,
        const Vector& unrealized_gains,
        const OptimizationParams& params
    ) const override;

    [[nodiscard]] std::string name() const override { return "osqp"; }
};

OptimizationResult OSQPOptimizer::solve(
    const Vector& benchmark_weights,
    const Vector& current_weights,
    const Matrix& cov,
    const Vector& unrealized_gains,
    const OptimizationParams& params
) const {
    const int N = static_cast<int>(benchmark_weights.size());
    const int n_vars = 3 * N;  // w (N) + b (N) + s (N)

    // -----------------------------------------------------------------------
    // Build P matrix (3N x 3N, upper triangular)
    // P = [2*λ_drift*Cov, 0, 0; 0, 0, 0; 0, 0, 0]
    // -----------------------------------------------------------------------
    std::vector<Eigen::Triplet<double>> P_triplets;
    P_triplets.reserve(N * N);
    for (int i = 0; i < N; ++i) {
        for (int j = i; j < N; ++j) {
            const double val = 2.0 * params.lambda_drift * cov(i, j);
            if (std::abs(val) > 1e-12) {
                P_triplets.emplace_back(i, j, val);
            }
        }
    }
    Eigen::SparseMatrix<double, Eigen::ColMajor, OSQPInt> P_eigen(n_vars, n_vars);
    P_eigen.setFromTriplets(P_triplets.begin(), P_triplets.end());
    P_eigen.makeCompressed();

    // -----------------------------------------------------------------------
    // Build q vector (3N)
    // q = [q_w; q_b; q_s]
    // q_w = -2 * λ_drift * Cov * w_ref
    // q_b[i] = λ_tcost + per_asset_tcost[i]
    // q_s[i] = λ_tcost + per_asset_tcost[i] + λ_tax * gain_penalty[i]
    // -----------------------------------------------------------------------
    Vector q(n_vars);
    q.head(N) = -2.0 * params.lambda_drift * cov * benchmark_weights;
    // Alpha term: -λ_alpha * α (minimize → negate to maximize expected return)
    if (params.alpha.size() == N && params.lambda_alpha > 0.0) {
        q.head(N) -= params.lambda_alpha * params.alpha;
    }
    for (int i = 0; i < N; ++i) {
        double per_asset_tcost = (params.tcost_frac.size() > i) ? params.tcost_frac(i) : 0.0;
        // Buy cost: transaction cost only
        q(N + i) = params.lambda_tcost + per_asset_tcost;
        // Sell cost: transaction cost + tax penalty (positive gains only)
        double gain_penalty = 0.0;
        if (unrealized_gains(i) > 0.0) {
            gain_penalty = params.lambda_tax * unrealized_gains(i) * params.tax_rate;
        }
        q(2 * N + i) = params.lambda_tcost + per_asset_tcost + gain_penalty;
    }

    // -----------------------------------------------------------------------
    // Build A matrix and bounds
    //
    // Row 0:           sum(w) = invest_fraction          (budget)
    // Rows 1..N:       lb <= w_i <= ub                   (weight bounds)
    // Rows N+1..2N:    w_i - b_i + s_i = w_c_i          (inventory balance)
    // Rows 2N+1..3N:   0 <= b_i <= bmax                  (buy bounds)
    // Rows 3N+1..4N:   0 <= s_i <= smax                  (sell bounds)
    // Row 4N+1:        0.5*sum(b+s) <= turnover_cap      (optional)
    // -----------------------------------------------------------------------
    const bool has_turnover = params.turnover_cap < 1.0;
    const int n_constraints = 1 + N + N + N + N + (has_turnover ? 1 : 0);
    std::vector<Eigen::Triplet<double>> A_triplets;

    // Row 0: sum(w) = invest_fraction
    for (int i = 0; i < N; ++i) {
        A_triplets.emplace_back(0, i, 1.0);
    }

    // Rows 1..N: w_i bounds (via l/u)
    for (int i = 0; i < N; ++i) {
        A_triplets.emplace_back(1 + i, i, 1.0);
    }

    // Rows N+1..2N: inventory balance: w_i - b_i + s_i = w_c_i
    for (int i = 0; i < N; ++i) {
        A_triplets.emplace_back(1 + N + i, i, 1.0);        // w_i
        A_triplets.emplace_back(1 + N + i, N + i, -1.0);   // -b_i
        A_triplets.emplace_back(1 + N + i, 2*N + i, 1.0);  // +s_i
    }

    // Rows 2N+1..3N: b_i bounds (via l/u)
    for (int i = 0; i < N; ++i) {
        A_triplets.emplace_back(1 + 2*N + i, N + i, 1.0);
    }

    // Rows 3N+1..4N: s_i bounds (via l/u)
    for (int i = 0; i < N; ++i) {
        A_triplets.emplace_back(1 + 3*N + i, 2*N + i, 1.0);
    }

    // Optional turnover: 0.5 * sum(b + s) <= turnover_cap
    if (has_turnover) {
        for (int i = 0; i < N; ++i) {
            A_triplets.emplace_back(1 + 4*N, N + i, 0.5);    // b_i
            A_triplets.emplace_back(1 + 4*N, 2*N + i, 0.5);  // s_i
        }
    }

    Eigen::SparseMatrix<double, Eigen::ColMajor, OSQPInt> A_eigen(n_constraints, n_vars);
    A_eigen.setFromTriplets(A_triplets.begin(), A_triplets.end());
    A_eigen.makeCompressed();

    // Bounds
    Vector l(n_constraints), u(n_constraints);

    // Row 0: budget
    l(0) = params.invest_fraction;
    u(0) = params.invest_fraction;

    // Rows 1..N: per-asset weight bounds
    for (int i = 0; i < N; ++i) {
        double lb = 0.0;
        double ub = params.weight_cap;
        auto it = params.per_asset_bounds.find(i);
        if (it != params.per_asset_bounds.end()) {
            lb = it->second.lo;
            ub = it->second.hi;
        }
        l(1 + i) = lb;
        u(1 + i) = ub;
    }

    // Rows N+1..2N: inventory balance (equality)
    for (int i = 0; i < N; ++i) {
        l(1 + N + i) = current_weights(i);
        u(1 + N + i) = current_weights(i);
    }

    // Rows 2N+1..3N: buy bounds
    for (int i = 0; i < N; ++i) {
        l(1 + 2*N + i) = 0.0;
        u(1 + 2*N + i) = params.no_buy.count(i) ? 0.0 : 1e30;
    }

    // Rows 3N+1..4N: sell bounds (capped at current weight for long-only)
    for (int i = 0; i < N; ++i) {
        l(1 + 3*N + i) = 0.0;
        u(1 + 3*N + i) = params.no_sell.count(i) ? 0.0 : current_weights(i);
    }

    // Turnover
    if (has_turnover) {
        l(1 + 4*N) = -1e30;
        u(1 + 4*N) = params.turnover_cap;
    }

    // -----------------------------------------------------------------------
    // Set up OSQP
    // -----------------------------------------------------------------------
    OSQPSolver* solver = nullptr;
    OSQPSettings settings;
    osqp_set_default_settings(&settings);
    settings.verbose = false;
    settings.eps_abs = 1e-6;
    settings.eps_rel = 1e-6;
    settings.max_iter = 10000;

    // Convert Eigen sparse to OSQP CSC format
    OSQPCscMatrix P_csc;
    P_csc.m = n_vars;
    P_csc.n = n_vars;
    P_csc.nzmax = P_eigen.nonZeros();
    P_csc.x = const_cast<double*>(P_eigen.valuePtr());
    P_csc.i = const_cast<OSQPInt*>(P_eigen.innerIndexPtr());
    P_csc.p = const_cast<OSQPInt*>(P_eigen.outerIndexPtr());
    P_csc.nz = -1;  // CSC format

    OSQPCscMatrix A_csc;
    A_csc.m = n_constraints;
    A_csc.n = n_vars;
    A_csc.nzmax = A_eigen.nonZeros();
    A_csc.x = const_cast<double*>(A_eigen.valuePtr());
    A_csc.i = const_cast<OSQPInt*>(A_eigen.innerIndexPtr());
    A_csc.p = const_cast<OSQPInt*>(A_eigen.outerIndexPtr());
    A_csc.nz = -1;

    OSQPInt exit_flag = osqp_setup(
        &solver,
        &P_csc,
        q.data(),
        &A_csc,
        l.data(),
        u.data(),
        n_constraints,
        n_vars,
        &settings
    );

    OptimizationResult result;
    result.converged = false;

    if (exit_flag != 0) {
        result.solver_status = "OSQP setup failed";
        return result;
    }

    // Solve
    exit_flag = osqp_solve(solver);

    if (exit_flag == 0 && solver->info->status_val == OSQP_SOLVED) {
        result.converged = true;
        result.target_weights = Eigen::Map<const Vector>(solver->solution->x, N);
        result.objective_value = solver->info->obj_val;
        result.solver_status = "solved";

        // Compute predicted TE
        const Vector active = result.target_weights - benchmark_weights;
        result.predicted_te = std::sqrt(
            std::max(0.0, static_cast<double>(active.transpose() * cov * active)) * 252.0
        );

        // Compute predicted turnover
        result.predicted_turnover = (result.target_weights - current_weights).cwiseAbs().sum() / 2.0;
    } else {
        result.solver_status = solver->info->status;
    }

    osqp_cleanup(solver);
    return result;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<Optimizer> make_optimizer(const std::string& name) {
    if (name == "osqp" || name.empty()) {
        return std::make_unique<OSQPOptimizer>();
    }
    throw std::invalid_argument("Unknown solver: " + name + ". Available: osqp");
}

} // namespace openvolt
