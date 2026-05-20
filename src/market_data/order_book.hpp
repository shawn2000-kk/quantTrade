#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include "common/types.hpp"
#include "market_data/market_data_types.hpp"

namespace hft {

// ── 无锁订单簿 ─────────────────────────────────────────────────────
//
// 设计约束：
//   - bids_ 按价格降序（bids_[0] = 最高买价），asks_ 按价格升序（asks_[0] = 最低卖价）
//   - 内部存储 20 档（bid/ask 各一个 std::array<PriceLevel,20>）
//   - snapshot_bbo / snapshot_depth 返回最多 MARKET_DEPTH(5) 档快照
//   - 用 std::atomic_flag spinlock 保护并发读写（读多写少场景）
//   - alignas(CACHELINE_SIZE) 保证对象首字节落在 cacheline 边界
//   - -fno-exceptions / -fno-rtti 兼容（无虚函数，无异常）

class alignas(CACHELINE_SIZE) OrderBook {
public:
    static constexpr uint8_t MAX_LEVELS = 20;  // 内部存储最大档位数

    explicit OrderBook(InstrumentId id) noexcept;

    // ── 写路径 ──────────────────────────────────────────────────────
    // apply_bbo：用 BBOEvent 中的 bid/ask 更新第 0 档（最优档）
    void apply_bbo(const BBOEvent& e) noexcept;

    // apply_depth：全量替换 bids_[0..bid_levels-1] 和 asks_[0..ask_levels-1]
    void apply_depth(const DepthEvent& e) noexcept;

    // ── 读路径（快照） ───────────────────────────────────────────────
    // 返回当前最优档，封装为 BBOEvent
    BBOEvent   snapshot_bbo()   const noexcept;

    // 返回当前 MARKET_DEPTH(5) 档，封装为 DepthEvent
    DepthEvent snapshot_depth() const noexcept;

    // ── 派生指标 ────────────────────────────────────────────────────
    Price mid_price()  const noexcept;   // (bid[0] + ask[0]) / 2；无有效档返回 0
    Price spread()     const noexcept;   // ask[0] - bid[0]；无有效档返回 0
    bool  is_crossed() const noexcept;   // bid[0].price >= ask[0].price → 行情异常

    InstrumentId instrument_id() const noexcept;

private:
    // ── RAII spinlock（不使用异常，不依赖 std::mutex）────────────────
    struct SpinGuard {
        std::atomic_flag& flag_;
        explicit SpinGuard(std::atomic_flag& f) noexcept;
        ~SpinGuard() noexcept;
        SpinGuard(const SpinGuard&)            = delete;
        SpinGuard& operator=(const SpinGuard&) = delete;
    };

    // ── 数据成员 ─────────────────────────────────────────────────────
    // 注意：instrument_id_ + bid_count_ + ask_count_ + _pad 共 8 字节，
    // 后续 bids_(320B) + asks_(320B) + lock_(1B) 共 641B；
    // alignas(64) 保证首字节对齐，整体大小不严格限制为 64 字节
    InstrumentId instrument_id_;
    uint8_t      bid_count_{0};    // 当前有效 bid 档位数（≤ MAX_LEVELS）
    uint8_t      ask_count_{0};    // 当前有效 ask 档位数（≤ MAX_LEVELS）
    uint8_t      _pad[2]{};

    // bids_[0] = 最高买价（降序），asks_[0] = 最低卖价（升序）
    std::array<PriceLevel, MAX_LEVELS> bids_{};
    std::array<PriceLevel, MAX_LEVELS> asks_{};

    // C++20：std::atomic_flag 默认构造即处于 clear 状态，无需 ATOMIC_FLAG_INIT
    mutable std::atomic_flag lock_{};
};

}  // namespace hft
