#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// ============================================================
// metrics_report.hpp — 回测绩效报告
//
// 用法：
//   MetricsReport report;
//   report.record_pnl(daily_pnl);   // 每个交易日调用一次
//   double sr = report.sharpe_ratio();
//   double dd = report.max_drawdown();
// ============================================================

namespace hft {

class MetricsReport {
public:
    MetricsReport() = default;

    // 禁止拷贝（持有 vector 成员，浅拷贝语义容易出错）
    MetricsReport(const MetricsReport&)            = delete;
    MetricsReport& operator=(const MetricsReport&) = delete;

    // ── 数据记录 ──────────────────────────────────────────────────

    /// 记录一个交易日的 P&L（tick * qty 单位，可正可负）
    void record_pnl(int64_t daily_pnl) noexcept;

    // ── 绩效指标 ──────────────────────────────────────────────────

    /// 年化 Sharpe = mean(daily_pnl) / std(daily_pnl) * sqrt(252)
    /// 样本 < 2 时返回 0.0（无法计算标准差）
    [[nodiscard]] double sharpe_ratio() const noexcept;

    /// 最大回撤幅度 [0.0, 1.0]，从累计盈亏曲线的峰值跌落的最大比例
    /// 累计盈亏从 0 起算；峰值 <= 0 时对应回撤为 0.0
    [[nodiscard]] double max_drawdown() const noexcept;

    /// 盈利天数（daily_pnl > 0）/ 总交易天数，无数据时返回 0.0
    [[nodiscard]] double win_rate() const noexcept;

    /// 累计 P&L（所有 daily_pnl 之和）
    [[nodiscard]] int64_t total_pnl() const noexcept;

    [[nodiscard]] int64_t max_daily_pnl() const noexcept;
    [[nodiscard]] int64_t min_daily_pnl() const noexcept;
    [[nodiscard]] size_t  trading_days()  const noexcept { return daily_pnls_.size(); }

private:
    std::vector<int64_t> daily_pnls_;
};

} // namespace hft
