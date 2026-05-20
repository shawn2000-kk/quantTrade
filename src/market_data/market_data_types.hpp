#pragma once
#include <type_traits>
#include "common/types.hpp"

namespace hft {

// 单个价格档位（订单簿内部 + DepthEvent 使用）
struct PriceLevel {
    Price price{0};
    Qty   qty{0};
};
static_assert(sizeof(PriceLevel) == 16);
static_assert(std::is_trivially_copyable_v<PriceLevel>);

// ── 热路径：BBOEvent ──────────────────────────────────────────
// 仅含 top-of-book，策略层 99% 的决策只需要这个
// alignas(64)：精确 1 个 cacheline，SPSC 传递零浪费
// 字段复用约定（节省空间，保持 64 字节）：
//   BBO_UPDATE / DEPTH_UPDATE：bid_px/ask_px 为最优买卖价，bid_qty/ask_qty 为对应量
//   TRADE：bid_px = 成交价，bid_qty = 成交量，ask_px/ask_qty = 0
struct alignas(CACHELINE_SIZE) BBOEvent {
    Timestamp    exchange_ts_ns{0};   // 交易所时间戳（纳秒）
    Timestamp    local_ts_ns{0};      // 本地 RDTSC 时间戳（纳秒）
    InstrumentId instrument_id{0};
    EventType    type{EventType::BBO_UPDATE};
    uint8_t      _pad[3]{};
    Price        bid_px{0};           // BBO: 最优买价；TRADE: 成交价
    Price        ask_px{0};           // BBO: 最优卖价；TRADE: 0
    Qty          bid_qty{0};          // BBO: 最优买量；TRADE: 成交量
    Qty          ask_qty{0};          // BBO: 最优卖量；TRADE: 0

    Price mid_price() const noexcept {
        if (bid_px > 0 && ask_px > 0) return (bid_px + ask_px) / 2;
        return 0;
    }
    Price spread() const noexcept {
        if (bid_px > 0 && ask_px > 0) return ask_px - bid_px;
        return 0;
    }
    bool is_valid_bbo() const noexcept {
        return bid_px > 0 && ask_px > 0 && bid_px < ask_px;
    }
};
static_assert(sizeof(BBOEvent) == 64, "BBOEvent must be exactly 1 cacheline");
static_assert(std::is_trivially_copyable_v<BBOEvent>, "BBOEvent must be trivially copyable for SPSC");

// ── 冷路径：DepthEvent ───────────────────────────────────────
// 完整 5 档深度，供做市策略计算 inventory skew、统计套利查深度使用
// 不走 SPSC 热队列，由 OrderBook 模块直接提供引用访问
struct DepthEvent {
    Timestamp    ts_ns{0};
    InstrumentId instrument_id{0};
    uint8_t      bid_levels{0};   // 实际有效买档数（≤ MARKET_DEPTH）
    uint8_t      ask_levels{0};   // 实际有效卖档数
    uint8_t      _pad[2]{};
    PriceLevel   bids[MARKET_DEPTH]{};
    PriceLevel   asks[MARKET_DEPTH]{};
};
static_assert(std::is_trivially_copyable_v<DepthEvent>);

} // namespace hft
