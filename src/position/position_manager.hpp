#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>
#include "common/types.hpp"
#include "oms/order.hpp"

namespace hft {

// PositionManager — 实时持仓管理
//
// 设计约束：
//   - 每个品种独占一个 cacheline（alignas(CACHELINE_SIZE) Slot），消除 false sharing
//   - 全部原子操作，无互斥锁，兼容 -fno-exceptions -fno-rtti
//   - 写入（on_fill）设计为单线程调用（PositionThread），读取可多线程
//   - avg_cost 始终以正整数 tick 单位表示成本价（多空均为正值）
//   - realized_pnl 以 tick * qty 为单位，可正可负
//
// 内存顺序策略：
//   - 写入时：avg_cost / realized_pnl 用 relaxed，net_qty 最后用 release
//     （net_qty 作为"提交"信号，读者 acquire 后可见前两个字段的最新值）
//   - 读取时：net_qty 用 acquire，其余字段用 relaxed

class PositionManager {
public:
    // Slot：每品种持仓数据，独占一个 cacheline
    struct alignas(CACHELINE_SIZE) Slot {
        std::atomic<Qty>   net_qty{0};       // 净持仓：buy+, sell-
        std::atomic<Price> avg_cost{0};      // 加权平均成本（整数 tick）
        std::atomic<Price> realized_pnl{0};  // 已实现盈亏（tick * qty）

        // 补齐至恰好一个 cacheline，防止相邻 Slot 共享 cacheline
        static constexpr size_t kAtomicSize = sizeof(std::atomic<int64_t>);
        uint8_t _pad[CACHELINE_SIZE - 3 * kAtomicSize];
    };
    static_assert(sizeof(Slot) == CACHELINE_SIZE,
                  "Slot must be exactly one cacheline to eliminate false sharing");
    static_assert(std::is_trivially_destructible_v<Slot>);

    PositionManager() = default;

    // 禁止拷贝：数组过大，且含 atomic 成员
    PositionManager(const PositionManager&)            = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    // ── 写接口（PositionThread 调用）────────────────────────────
    //
    // on_fill 处理逻辑（整数 tick 单位）：
    //
    //  1. 净持仓为 0（flat）→ 开仓，avg_cost = fill_price
    //  2. 同向加仓（多头 BUY / 空头 SELL）→ avg_cost 加权平均
    //  3. 反向减仓（多头 SELL / 空头 BUY）→ 计算 realized_pnl，avg_cost 不变
    //  4. 反向超额（过零点翻仓）→ 先平旧仓计算 realized_pnl，
    //                              再以 fill_price 开新仓
    void on_fill(const FillEvent& fill) noexcept;

    // ── 读接口（可多线程）─────────────────────────────────────────
    Qty   get_net_qty(InstrumentId id)        const noexcept;
    Price get_avg_cost(InstrumentId id)       const noexcept;
    Price get_realized_pnl(InstrumentId id)   const noexcept;

    // 所有品种已实现盈亏求和（监控路径，非热路径）
    Price get_total_realized_pnl() const noexcept;

    // 提供只读 Slot 引用供 PnlCalculator 使用，避免重复封装
    const Slot& slot(InstrumentId id) const noexcept { return slots_[id]; }

private:
    // MAX_INSTRUMENTS 个 Slot，紧凑排列，每个独占一个 cacheline
    // 静态数组：无动态分配，启动时已完全初始化为 0
    Slot slots_[MAX_INSTRUMENTS];
};

} // namespace hft
