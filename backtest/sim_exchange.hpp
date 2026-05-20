#pragma once

#include <cstdint>
#include <queue>
#include <string_view>

#include "connectivity/gateway.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"

// ============================================================
// sim_exchange.hpp — 模拟交易所（回测专用，继承 IGateway）
//
// 撮合逻辑：
//   - 限价买单：ask_px <= order.price + slippage_ticks → 成交
//   - 限价卖单：bid_px >= order.price - slippage_ticks → 成交
//   - fill_ratio 控制成交比例（1.0=全成，0.0=不成交）
//   - is_connected() 恒返回 true，get_rtt_ns() 返回 500000ns（500µs 模拟延迟）
// ============================================================

namespace hft {

class SimExchange : public IGateway {
public:
    /// @param slippage_ticks  允许的滑点（tick 单位，正值放宽成交条件）
    /// @param fill_ratio      成交比例 [0.0, 1.0]，1.0 = 全成，0.0 = 不成交
    explicit SimExchange(double slippage_ticks = 0.0,
                         double fill_ratio     = 1.0) noexcept;

    // ── IGateway 实现 ─────────────────────────────────────────────

    /// 接受报单，分配 client_order_id + exchange_order_id，推入 pending_orders_
    void send_new_order(const OrderRequest& req) noexcept override;

    /// 空操作（回测不维护撤单逻辑）
    void send_cancel(uint64_t client_order_id) noexcept override;

    /// 空操作（回测不维护改单逻辑）
    void send_modify(const ModifyRequest& req) noexcept override;

    [[nodiscard]] bool        is_connected() const noexcept override { return true; }
    [[nodiscard]] uint64_t    get_rtt_ns()   const noexcept override { return 500'000; }
    [[nodiscard]] std::string_view name()    const noexcept override { return "SimExchange"; }

    // ── 行情驱动撮合 ───────────────────────────────────────────────

    /// 每来一笔 BBO，尝试撮合所有 pending 订单
    /// 成交则生成 FillEvent → fill_queue_ 和 OrderAck → ack_queue_
    void process_bbo(const BBOEvent& bbo) noexcept;

    // ── 消费回报队列 ───────────────────────────────────────────────

    bool pop_ack (OrderAck&  out) noexcept;
    bool pop_fill(FillEvent& out) noexcept;

    [[nodiscard]] size_t pending_count() const noexcept { return pending_orders_.size(); }

private:
    // 挂单内部表示（含已分配的 ID）
    struct PendingEntry {
        OrderRequest req;
        uint64_t     arrive_ts_ns;
        uint64_t     client_order_id;
        uint64_t     exchange_order_id;
    };

    double   slippage_ticks_;
    double   fill_ratio_;

    uint64_t next_client_order_id_{1};
    uint64_t next_exchange_order_id_{1};

    std::queue<PendingEntry> pending_orders_;
    std::queue<OrderAck>     ack_queue_;
    std::queue<FillEvent>    fill_queue_;
};

} // namespace hft
