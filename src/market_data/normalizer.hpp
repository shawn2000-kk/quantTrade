#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include "common/types.hpp"           // MARKET_DEPTH
#include "market_data/market_data_types.hpp"  // BBOEvent, DepthEvent, PriceLevel

namespace hft {

// ── 交易所类型枚举 ──────────────────────────────────────────────────────
enum class ExchangeType : uint8_t {
    INTERNAL  = 0,  // 内部标准二进制格式（FeedHandler 默认）
    BINANCE   = 1,  // Binance 现货/期货简化 UDP 格式（测试用）
    CFFEX     = 2,  // 中金所 BINARY 格式
    SSE_STEP  = 3,  // 上交所 STEP/BINARY
};

// ── 内部标准二进制 Wire Format ──────────────────────────────────────────
// FeedHandler 和 PcapReplayer 都生成/解析这个格式
// 单个 UDP 包可包含多个连续 WireMsg（变长）
// 设计：紧凑固定长度（48 字节），避免动态解析

inline constexpr uint32_t WIRE_MAGIC    = 0x48465420u;  // "HFT "
inline constexpr uint8_t  WIRE_BBO     = 0x01;
inline constexpr uint8_t  WIRE_TRADE   = 0x02;
inline constexpr uint8_t  WIRE_DEPTH   = 0x03;

#pragma pack(push, 1)
struct WireBBO {
    uint32_t magic;           // WIRE_MAGIC
    uint8_t  msg_type;        // WIRE_BBO 或 WIRE_TRADE
    uint8_t  _pad[3];
    uint32_t instrument_id;
    uint64_t exchange_ts_ns;
    int64_t  bid_price;       // TRADE 时为成交价
    int64_t  ask_price;       // TRADE 时为 0
    int64_t  bid_qty;         // TRADE 时为成交量
    int64_t  ask_qty;         // TRADE 时为 0
};  // 4+4+4+8+8+8+8+8 = 52 bytes… 对齐到 56
static_assert(sizeof(WireBBO) == 52);

struct WireDepthLevel {
    int64_t price;
    int64_t qty;
};
static_assert(sizeof(WireDepthLevel) == 16);

struct WireDepth {
    uint32_t magic;
    uint8_t  msg_type;        // WIRE_DEPTH
    uint8_t  bid_levels;
    uint8_t  ask_levels;
    uint8_t  _pad;
    uint32_t instrument_id;
    uint64_t exchange_ts_ns;
    WireDepthLevel bids[MARKET_DEPTH];
    WireDepthLevel asks[MARKET_DEPTH];
};  // 4+4+4+8 + 5*16*2 = 20+160 = 180 bytes
static_assert(sizeof(WireDepth) == 180);
#pragma pack(pop)

// ── Normalizer ──────────────────────────────────────────────────────────
//
// 职责：将原始网络字节流（任意交易所格式）解析为内部 BBOEvent / DepthEvent
// 无状态：所有解析函数是纯函数（static），不保存跨消息状态
// 扩展：新增交易所只需在 parse_xxx() 系列添加 case

class Normalizer {
public:
    Normalizer()  = default;
    ~Normalizer() = default;

    // 解析单条消息，返回 std::optional（格式错误时返回 nullopt）
    // buf / len：原始网络字节，不要求对齐
    // exchange：告知 Normalizer 用哪种格式解析
    [[nodiscard]] static std::optional<BBOEvent>
    parse_bbo(const uint8_t* buf, size_t len,
              ExchangeType exchange = ExchangeType::INTERNAL) noexcept;

    [[nodiscard]] static std::optional<DepthEvent>
    parse_depth(const uint8_t* buf, size_t len,
                ExchangeType exchange = ExchangeType::INTERNAL) noexcept;

    // 快速判断 buf 是否为 BBO 消息（检查 magic + msg_type）
    [[nodiscard]] static bool is_bbo(const uint8_t* buf, size_t len) noexcept;
    [[nodiscard]] static bool is_depth(const uint8_t* buf, size_t len) noexcept;

    // 从单个 UDP 包（可含多条 WireBBO 消息）批量解析，写入 out_buf
    // 返回实际解析到的 BBOEvent 数量（≤ max_out）
    static size_t parse_batch(const uint8_t* buf, size_t len,
                              BBOEvent* out_buf, size_t max_out,
                              ExchangeType exchange = ExchangeType::INTERNAL) noexcept;

private:
    static std::optional<BBOEvent>
    parse_internal_bbo(const uint8_t* buf, size_t len) noexcept;

    static std::optional<DepthEvent>
    parse_internal_depth(const uint8_t* buf, size_t len) noexcept;

    // Binance 简化 UDP 格式：用于测试/回测数据生成
    static std::optional<BBOEvent>
    parse_binance_bbo(const uint8_t* buf, size_t len) noexcept;
};

}  // namespace hft
