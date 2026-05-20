#pragma once
#include <atomic>
#include <cstdint>
#include "common/types.hpp"

namespace hft {

// 持仓限额检查（Pre-trade CAS 预留 + 成交/撤单修正）
class PositionLimits {
public:
    // max_net_qty:  每个品种最大净持仓（手/股，绝对值）
    // max_notional: 单笔报单最大名义金额（price × qty，整数）
    PositionLimits(Qty max_net_qty, Price max_notional) noexcept;

    // Pre-trade 检查并预留持仓额度
    //   1. 检查 price × qty <= max_notional
    //   2. CAS 尝试更新 net_qty（BUY +qty，SELL -qty），超过 ±max_net_qty 则拒绝
    // 成功返回 true，并持久更新内部 net_qty（相当于 reserve）
    [[nodiscard]] bool check_and_reserve(
        InstrumentId id, Side side, Qty qty, Price price) noexcept;

    // 成交回报修正：on_fill 无需额外操作（check_and_reserve 已原子更新持仓）
    // 此接口留作未来需要"预留 vs 实际"差值修正时使用
    void on_fill(InstrumentId id, Side side, Qty fill_qty) noexcept;

    // 撤单：归还 check_and_reserve 预留的额度
    void on_cancel(InstrumentId id, Side side, Qty qty) noexcept;

    // 查询当前净持仓（正多负空）
    [[nodiscard]] Qty get_net_qty(InstrumentId id) const noexcept;

private:
    // 每个品种独占一个 cacheline，避免 false sharing
    struct alignas(64) Slot {
        std::atomic<Qty> v{0};
    };
    Slot   slots_[MAX_INSTRUMENTS];
    Qty    max_net_qty_;
    Price  max_notional_;
};

} // namespace hft
