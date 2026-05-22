#pragma once
#include <atomic>
#include <cstdint>
#include "common/types.hpp"
#include "market_data/market_data_types.hpp"
#include "strategy/strategy_base.hpp"
#include "oms/order.hpp"

namespace hft {

// ── 动量策略（EMA 交叉）──────────────────────────────────────────────────
//
// 信号：
//   fast_ema：短周期指数移动平均（默认 8 个 tick）
//   slow_ema：长周期指数移动平均（默认 21 个 tick）
//
//   fast > slow + signal_band → 上穿，开多 / 平空
//   fast < slow - signal_band → 下穿，开空 / 平多
//   信号消失（|fast - slow| < signal_band）→ 平仓
//
// 执行：
//   仅使用 mid_price 触发信号；报单为 IOC，立即成交或撤销
//   信号强度：(fast - slow) / slow × 10000（万分比）
//
// 风险：
//   - 一次只持一个方向，不加仓
//   - 持仓后若价差反向超过 max_adverse_ticks，止损平仓
//
// 线程安全：
//   - on_bbo_impl 在策略线程调用
//   - inventory_ 为 atomic，on_fill_impl 在 OMS/持仓线程写入

class Momentum : public StrategyBase<Momentum> {
public:
    // fast_period    : 快线 EMA 周期（样本数）
    // slow_period    : 慢线 EMA 周期（样本数）
    // order_qty      : 每次报单数量
    // signal_band    : 过滤噪声的最小价差（tick 数）
    // max_adverse    : 止损：反向运动超过此 tick 数触发平仓
    Momentum(InstrumentId instrument_id,
             int          fast_period    = 8,
             int          slow_period    = 21,
             Qty          order_qty      = 100,
             Price        signal_band    = 1,
             Price        max_adverse    = 10) noexcept;

    // ── CRTP 回调 ───────────────────────────────────────────────────
    void on_bbo_impl(const BBOEvent& e) noexcept;
    void on_fill_impl(const FillEvent& f) noexcept;
    void on_order_ack_impl(const OrderAck& a) noexcept;

    // OMS 接口开放
    using StrategyBase<Momentum>::pop_order;

    // ── 诊断 ────────────────────────────────────────────────────────
    [[nodiscard]] double fast_ema()  const noexcept { return fast_ema_; }
    [[nodiscard]] double slow_ema()  const noexcept { return slow_ema_; }
    [[nodiscard]] Qty    inventory()  const noexcept {
        return inventory_.load(std::memory_order_relaxed);
    }

private:
    enum class Signal : uint8_t { NONE = 0, LONG = 1, SHORT = 2 };

    // EMA 更新：alpha = 2 / (period + 1)
    static double ema_update(double prev, double price, double alpha) noexcept;

    void update_ema(double mid) noexcept;
    Signal compute_signal() const noexcept;
    void   open_position(Signal sig) noexcept;
    void   close_position() noexcept;
    void   check_stop_loss(Price mid) noexcept;

    double fast_alpha_;
    double slow_alpha_;
    double fast_ema_{0.0};
    double slow_ema_{0.0};
    int    warmup_count_{0};
    int    warmup_needed_;

    Qty   order_qty_;
    Price signal_band_;
    Price max_adverse_;

    Signal last_signal_{Signal::NONE};
    Price  entry_price_{0};    // 开仓时的 mid price（止损参考）

    std::atomic<Qty> inventory_{0};   // 净持仓（正多负空）
};

static_assert(!std::is_polymorphic_v<Momentum>,
    "Momentum must not have a vtable");

}  // namespace hft
