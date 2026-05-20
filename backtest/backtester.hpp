#pragma once

#include <cstdint>
#include <functional>

#include "backtest/sim_exchange.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"
#include "position/position_manager.hpp"

// ============================================================
// backtester.hpp — 回测引擎主控
//
// 职责：
//   1. 接受 BBOEvent 驱动，调用 SimExchange 撮合
//   2. 消费所有 ACK → 推给注册的 ack_handler
//   3. 消费所有 FillEvent → 更新 PositionManager + 推给 fill_handler
//   4. 统计已处理 BBO 数量和总成交笔数
// ============================================================

namespace hft {

class Backtester {
public:
    explicit Backtester(SimExchange&     exchange,
                        PositionManager& pm) noexcept;

    // 禁止拷贝（持有引用成员）
    Backtester(const Backtester&)            = delete;
    Backtester& operator=(const Backtester&) = delete;

    // ── 行情驱动入口 ──────────────────────────────────────────────

    /// 喂入一条 BBO，触发撮合并处理所有回报
    void feed_bbo(const BBOEvent& bbo) noexcept;

    // ── 回调注册 ──────────────────────────────────────────────────

    void set_ack_handler (std::function<void(const OrderAck&)>   handler) noexcept;
    void set_fill_handler(std::function<void(const FillEvent&)>  handler) noexcept;

    // ── 统计查询 ──────────────────────────────────────────────────

    [[nodiscard]] uint64_t processed_bbo_count() const noexcept { return bbo_count_;  }
    [[nodiscard]] uint64_t total_fills()          const noexcept { return fill_count_; }

private:
    SimExchange&     exchange_;
    PositionManager& pm_;

    std::function<void(const OrderAck&)>  ack_handler_;
    std::function<void(const FillEvent&)> fill_handler_;

    uint64_t bbo_count_{0};
    uint64_t fill_count_{0};
};

} // namespace hft
