#include "market_data/normalizer.hpp"
#include <cstring>

namespace hft {

// ── 内部格式解析 ────────────────────────────────────────────────────────

std::optional<BBOEvent>
Normalizer::parse_internal_bbo(const uint8_t* buf, size_t len) noexcept {
    if (len < sizeof(WireBBO)) return std::nullopt;

    WireBBO wire{};
    std::memcpy(&wire, buf, sizeof(WireBBO));

    if (wire.magic != WIRE_MAGIC) return std::nullopt;
    if (wire.msg_type != WIRE_BBO && wire.msg_type != WIRE_TRADE) return std::nullopt;

    BBOEvent e{};
    e.exchange_ts_ns = static_cast<Timestamp>(wire.exchange_ts_ns);
    e.local_ts_ns    = e.exchange_ts_ns;   // 由 FeedHandler 覆盖为 RDTSC 值
    e.instrument_id  = wire.instrument_id;
    e.type           = (wire.msg_type == WIRE_TRADE)
                       ? EventType::TRADE : EventType::BBO_UPDATE;
    e.bid_px  = wire.bid_price;
    e.ask_px  = wire.ask_price;
    e.bid_qty = wire.bid_qty;
    e.ask_qty = wire.ask_qty;
    return e;
}

std::optional<DepthEvent>
Normalizer::parse_internal_depth(const uint8_t* buf, size_t len) noexcept {
    if (len < sizeof(WireDepth)) return std::nullopt;

    WireDepth wire{};
    std::memcpy(&wire, buf, sizeof(WireDepth));

    if (wire.magic != WIRE_MAGIC) return std::nullopt;
    if (wire.msg_type != WIRE_DEPTH) return std::nullopt;

    DepthEvent e{};
    e.ts_ns         = static_cast<Timestamp>(wire.exchange_ts_ns);
    e.instrument_id = wire.instrument_id;
    e.bid_levels    = wire.bid_levels < MARKET_DEPTH
                      ? wire.bid_levels : static_cast<uint8_t>(MARKET_DEPTH);
    e.ask_levels    = wire.ask_levels < MARKET_DEPTH
                      ? wire.ask_levels : static_cast<uint8_t>(MARKET_DEPTH);

    for (uint8_t i = 0; i < e.bid_levels; ++i) {
        e.bids[i].price = wire.bids[i].price;
        e.bids[i].qty   = wire.bids[i].qty;
    }
    for (uint8_t i = 0; i < e.ask_levels; ++i) {
        e.asks[i].price = wire.asks[i].price;
        e.asks[i].qty   = wire.asks[i].qty;
    }
    return e;
}

// ── Binance 简化 UDP 格式（与内部格式相同，仅 magic 不同，测试用）──────

std::optional<BBOEvent>
Normalizer::parse_binance_bbo(const uint8_t* buf, size_t len) noexcept {
    // 复用内部解析路径（Binance 测试数据生成时用相同结构，不检查 magic）
    if (len < sizeof(WireBBO)) return std::nullopt;

    WireBBO wire{};
    std::memcpy(&wire, buf, sizeof(WireBBO));
    if (wire.msg_type != WIRE_BBO && wire.msg_type != WIRE_TRADE) return std::nullopt;

    BBOEvent e{};
    e.exchange_ts_ns = static_cast<Timestamp>(wire.exchange_ts_ns);
    e.local_ts_ns    = e.exchange_ts_ns;
    e.instrument_id  = wire.instrument_id;
    e.type           = (wire.msg_type == WIRE_TRADE)
                       ? EventType::TRADE : EventType::BBO_UPDATE;
    e.bid_px  = wire.bid_price;
    e.ask_px  = wire.ask_price;
    e.bid_qty = wire.bid_qty;
    e.ask_qty = wire.ask_qty;
    return e;
}

// ── 公开接口 ────────────────────────────────────────────────────────────

std::optional<BBOEvent>
Normalizer::parse_bbo(const uint8_t* buf, size_t len,
                      ExchangeType exchange) noexcept {
    switch (exchange) {
        case ExchangeType::INTERNAL:
        case ExchangeType::CFFEX:
        case ExchangeType::SSE_STEP:
            return parse_internal_bbo(buf, len);
        case ExchangeType::BINANCE:
            return parse_binance_bbo(buf, len);
    }
    return std::nullopt;
}

std::optional<DepthEvent>
Normalizer::parse_depth(const uint8_t* buf, size_t len,
                        ExchangeType exchange) noexcept {
    switch (exchange) {
        case ExchangeType::INTERNAL:
        case ExchangeType::CFFEX:
        case ExchangeType::SSE_STEP:
        case ExchangeType::BINANCE:
            return parse_internal_depth(buf, len);
    }
    return std::nullopt;
}

bool Normalizer::is_bbo(const uint8_t* buf, size_t len) noexcept {
    if (len < sizeof(WireBBO)) return false;
    uint8_t msg_type = buf[4];  // offset of msg_type in WireBBO
    return (msg_type == WIRE_BBO || msg_type == WIRE_TRADE);
}

bool Normalizer::is_depth(const uint8_t* buf, size_t len) noexcept {
    if (len < sizeof(WireDepth)) return false;
    uint8_t msg_type = buf[4];
    return (msg_type == WIRE_DEPTH);
}

size_t Normalizer::parse_batch(const uint8_t* buf, size_t len,
                               BBOEvent* out_buf, size_t max_out,
                               ExchangeType exchange) noexcept {
    size_t count  = 0;
    size_t offset = 0;

    while (offset < len && count < max_out) {
        if (len - offset < sizeof(WireBBO)) break;

        const uint8_t* msg = buf + offset;
        const uint8_t  msg_type = msg[4];

        if (msg_type == WIRE_BBO || msg_type == WIRE_TRADE) {
            if (auto e = parse_bbo(msg, len - offset, exchange)) {
                out_buf[count++] = *e;
            }
            offset += sizeof(WireBBO);
        } else if (msg_type == WIRE_DEPTH) {
            // Depth 消息跳过（交由 parse_depth 单独处理）
            offset += sizeof(WireDepth);
        } else {
            // 未知消息类型：无法确定长度，停止解析
            break;
        }
    }
    return count;
}

}  // namespace hft
