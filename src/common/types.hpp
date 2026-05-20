#pragma once
#include <cstdint>
#include <cstddef>

namespace hft {

// --- 基础类型别名 ---
using Price        = int64_t;   // 价格：整数，单位为最小价格变动单位（tick）
using Qty          = int64_t;   // 数量：整数，单位为手/股/张
using InstrumentId = uint32_t;  // 品种 ID：运行时分配，通过配置文件映射到交易所代码
using Timestamp    = uint64_t;  // 时间戳：RDTSC 纳秒（本地）或交易所纳秒

// --- 行情事件类型 ---
enum class EventType : uint8_t {
    TRADE        = 0,   // 逐笔成交
    BBO_UPDATE   = 1,   // 最优买卖价更新
    DEPTH_UPDATE = 2,   // 深度行情更新
};

// --- 买卖方向 ---
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1,
};

// --- 订单类型 ---
enum class OrderType : uint8_t {
    MARKET = 0,   // 市价单
    LIMIT  = 1,   // 限价单
    IOC    = 2,   // Immediate-or-Cancel
    FOK    = 3,   // Fill-or-Kill
    FAK    = 4,   // Fill-and-Kill（部分成交剩余撤销）
};

// --- 订单状态 ---
enum class OrderStatus : uint8_t {
    PENDING_NEW      = 0,   // 已发出，等待交易所 ACK
    NEW              = 1,   // 交易所已确认
    PARTIALLY_FILLED = 2,   // 部分成交
    FILLED           = 3,   // 全部成交
    PENDING_CANCEL   = 4,   // 撤单请求已发出
    CANCELLED        = 5,   // 已撤销
    REJECTED         = 6,   // 被拒绝（风控或交易所）
};

// --- 熔断器状态 ---
enum class CBState : uint8_t {
    CLOSED    = 0,  // 正常，允许报单
    OPEN      = 1,  // 熔断中，拒绝所有报单
    HALF_OPEN = 2,  // 探测恢复中
};

// --- 系统常量 ---
inline constexpr size_t MARKET_DEPTH    = 5;     // 默认行情深度（档位数）
inline constexpr size_t MAX_INSTRUMENTS = 4096;  // 最大支持品种数
inline constexpr size_t CACHELINE_SIZE  = 64;    // cacheline 字节数

} // namespace hft
