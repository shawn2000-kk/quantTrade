#include "backtest/metrics_report.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hft {

void MetricsReport::record_pnl(int64_t daily_pnl) noexcept {
    daily_pnls_.push_back(daily_pnl);
}

double MetricsReport::sharpe_ratio() const noexcept {
    const size_t n = daily_pnls_.size();
    if (n < 2) return 0.0;

    // 计算均值
    double sum = 0.0;
    for (int64_t v : daily_pnls_) sum += static_cast<double>(v);
    const double mean = sum / static_cast<double>(n);

    // 样本标准差（除以 n-1）
    double sq_sum = 0.0;
    for (int64_t v : daily_pnls_) {
        const double diff = static_cast<double>(v) - mean;
        sq_sum += diff * diff;
    }
    const double std_dev = std::sqrt(sq_sum / static_cast<double>(n - 1));

    if (std_dev < 1e-12) return 0.0; // 避免除以零

    // 年化 Sharpe（假设 252 个交易日）
    return (mean / std_dev) * std::sqrt(252.0);
}

double MetricsReport::max_drawdown() const noexcept {
    if (daily_pnls_.empty()) return 0.0;

    double peak        = 0.0; // 累计 P&L 从 0 起算
    double cumulative  = 0.0;
    double max_dd      = 0.0;

    for (int64_t v : daily_pnls_) {
        cumulative += static_cast<double>(v);
        if (cumulative > peak) {
            peak = cumulative;
        }
        if (peak > 0.0) {
            const double dd = (peak - cumulative) / peak;
            if (dd > max_dd) max_dd = dd;
        }
    }
    return max_dd;
}

double MetricsReport::win_rate() const noexcept {
    if (daily_pnls_.empty()) return 0.0;
    size_t wins = 0;
    for (int64_t v : daily_pnls_) {
        if (v > 0) ++wins;
    }
    return static_cast<double>(wins) / static_cast<double>(daily_pnls_.size());
}

int64_t MetricsReport::total_pnl() const noexcept {
    int64_t total = 0;
    for (int64_t v : daily_pnls_) total += v;
    return total;
}

int64_t MetricsReport::max_daily_pnl() const noexcept {
    if (daily_pnls_.empty()) return 0;
    int64_t m = std::numeric_limits<int64_t>::min();
    for (int64_t v : daily_pnls_) if (v > m) m = v;
    return m;
}

int64_t MetricsReport::min_daily_pnl() const noexcept {
    if (daily_pnls_.empty()) return 0;
    int64_t m = std::numeric_limits<int64_t>::max();
    for (int64_t v : daily_pnls_) if (v < m) m = v;
    return m;
}

} // namespace hft
