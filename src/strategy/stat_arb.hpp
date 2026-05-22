#pragma once
#include <atomic>
#include <cstdint>
#include "common/types.hpp"
#include "market_data/market_data_types.hpp"
#include "strategy/strategy_base.hpp"
#include "oms/order.hpp"

namespace hft {

// ── 统计套利策略 ───────────────────────────────────────────────────────
//
// 原理：两个高度相关品种（如 A 股和其对应 ETF）价差均值回归
//
// 信号计算：
//   spread      = price_A - hedge_ratio * price_B
//   z_score     = (spread - rolling_mean) / rolling_std
//
// 开仓：
//   z_score > +entry_z → 价差偏高 → 卖 A，买 B（Short Spread）
//   z_score < -entry_z → 价差偏低 → 买 A，卖 B（Long Spread）
//
// 平仓：
//   |z_score| < exit_z → 价差回归均值，双腿同时平仓
//
// 止损：
//   |z_score| > stop_z → 价差继续发散，强制平仓
//
// 线程安全：
//   - on_bbo_impl 由策略线程调用（两个品种的 BBO 分别到达，用 last_bbo_[] 缓存）
//   - inventory_a_ / inventory_b_ 为 atomic，供 on_fill_impl 从 OMS 线程更新

class StatArb : public StrategyBase<StatArb> {
public:
    // instrument_a     : 第一条腿品种 ID
    // instrument_b     : 第二条腿品种 ID
    // hedge_ratio      : 对冲比例（B 的价格乘数，通常为协整回归系数）
    // order_qty        : 每腿报单数量
    // entry_z          : 开仓 z-score 阈值（通常 1.5 ~ 2.0）
    // exit_z           : 平仓 z-score 阈值（通常 0.5 ~ 0.0）
    // stop_z           : 止损 z-score 阈值（通常 3.0 ~ 4.0）
    // lookback         : 滚动统计窗口（价差样本数）
    StatArb(InstrumentId instrument_a,
            InstrumentId instrument_b,
            double       hedge_ratio,
            Qty          order_qty,
            double       entry_z  = 2.0,
            double       exit_z   = 0.5,
            double       stop_z   = 3.5,
            size_t       lookback = 100) noexcept;

    // ── CRTP 回调 ───────────────────────────────────────────────────
    void on_bbo_impl(const BBOEvent& e) noexcept;
    void on_fill_impl(const FillEvent& f) noexcept;
    void on_order_ack_impl(const OrderAck& a) noexcept;

    // OMS 接口开放
    using StrategyBase<StatArb>::pop_order;

    // ── 只读诊断 ────────────────────────────────────────────────────
    [[nodiscard]] double current_zscore() const noexcept;
    [[nodiscard]] int    position_state() const noexcept;  // -1=Short,0=Flat,+1=Long

private:
    // 状态机
    enum class ArbState : uint8_t { FLAT = 0, LONG_SPREAD = 1, SHORT_SPREAD = 2 };

    // 价差滚动统计（在线 Welford 算法，O(1) 每样本）
    struct WelfordStat {
        double mean{0.0};
        double m2{0.0};    // 方差的无偏累积量
        size_t count{0};
        size_t window;

        // 固定窗口近似：超过 window 后用指数衰减权重
        void update(double x) noexcept;
        [[nodiscard]] double variance() const noexcept;
        [[nodiscard]] double stddev()   const noexcept;
    };

    void try_open() noexcept;
    void try_close() noexcept;
    void send_pair(Side side_a, Side side_b) noexcept;

    InstrumentId instrument_b_{0};   // instrument_a_ 由 StrategyBase::instrument_id_ 持有
    double       hedge_ratio_;
    Qty          order_qty_;
    double       entry_z_;
    double       exit_z_;
    double       stop_z_;

    // 最近收到的两腿 BBO（缓存，等两腿都有数据才计算）
    Price last_mid_a_{0};
    Price last_mid_b_{0};

    WelfordStat stat_;
    double      current_z_{0.0};

    ArbState state_{ArbState::FLAT};

    std::atomic<Qty> inventory_a_{0};
    std::atomic<Qty> inventory_b_{0};
};

static_assert(!std::is_polymorphic_v<StatArb>,
    "StatArb must not have a vtable");

}  // namespace hft
