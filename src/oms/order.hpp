#pragma once
#include <type_traits>
#include "common/types.hpp"

namespace hft {

// 策略 → OMS：报单请求（轻量，走 SPSC 队列热路径）
struct OrderRequest {
    InstrumentId instrument_id{0};
    Side         side{Side::BUY};
    OrderType    type{OrderType::LIMIT};
    uint8_t      _pad[1]{};
    Price        price{0};             // MARKET 单填 0
    Qty          qty{0};
    Timestamp    strategy_ts_ns{0};    // 策略决策时的 TSC 时间戳（延迟统计用）
};
static_assert(sizeof(OrderRequest) == 32, "OrderRequest size mismatch");
static_assert(std::is_trivially_copyable_v<OrderRequest>);

// OMS 内部订单对象（由 MemoryPool<Order, N> 分配，零 new/delete）
// alignas(64)：独占 1 个 cacheline，OMS 热查找路径不跨 cacheline
struct alignas(CACHELINE_SIZE) Order {
    uint64_t     client_order_id{0};       // 单调递增，OMS 生成
    uint64_t     exchange_order_id{0};     // 交易所回报，ACK 前为 0
    InstrumentId instrument_id{0};
    Side         side{Side::BUY};
    OrderType    type{OrderType::LIMIT};
    OrderStatus  status{OrderStatus::PENDING_NEW};
    uint8_t      _pad[1]{};
    Price        price{0};
    Qty          qty{0};                   // 原始报单数量
    Qty          filled_qty{0};            // 累计成交数量
    Timestamp    submit_ts_ns{0};          // 报单发出时的 TSC 时间戳
    Timestamp    ack_ts_ns{0};             // 收到 ACK 时的 TSC 时间戳

    Qty remaining_qty() const noexcept { return qty - filled_qty; }
    bool is_active() const noexcept {
        return status == OrderStatus::PENDING_NEW
            || status == OrderStatus::NEW
            || status == OrderStatus::PARTIALLY_FILLED
            || status == OrderStatus::PENDING_CANCEL;
    }
    bool is_terminal() const noexcept {
        return status == OrderStatus::FILLED
            || status == OrderStatus::CANCELLED
            || status == OrderStatus::REJECTED;
    }
};
static_assert(sizeof(Order) == 64, "Order must fit in exactly 1 cacheline");
static_assert(std::is_trivially_copyable_v<Order>);

// 交易所 → OMS：报单确认
struct OrderAck {
    uint64_t     client_order_id{0};
    uint64_t     exchange_order_id{0};     // REJECTED 时为 0
    OrderStatus  status{OrderStatus::NEW}; // NEW 或 REJECTED
    uint8_t      _pad[7]{};
    Timestamp    ack_ts_ns{0};
};
static_assert(std::is_trivially_copyable_v<OrderAck>);

// 交易所 → OMS → 持仓层 / 策略层：成交回报
// alignas(64)：通过 SPSC 传递，独占 cacheline
struct alignas(CACHELINE_SIZE) FillEvent {
    uint64_t     client_order_id{0};
    uint64_t     exchange_order_id{0};
    InstrumentId instrument_id{0};
    Side         side{Side::BUY};
    uint8_t      _pad[3]{};
    Price        fill_price{0};            // 本次成交价格
    Qty          fill_qty{0};              // 本次成交数量
    Qty          remaining_qty{0};         // 成交后剩余数量（0 表示全部成交）
    Timestamp    fill_ts_ns{0};            // 成交时间戳
};
static_assert(sizeof(FillEvent) == 64, "FillEvent must fit in exactly 1 cacheline");
static_assert(std::is_trivially_copyable_v<FillEvent>);

// 执行层 → Gateway：改单请求
struct ModifyRequest {
    uint64_t client_order_id{0};
    Price    new_price{0};
    Qty      new_qty{0};
};
static_assert(std::is_trivially_copyable_v<ModifyRequest>);

} // namespace hft
