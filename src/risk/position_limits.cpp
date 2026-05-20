#include "risk/position_limits.hpp"

namespace hft {

PositionLimits::PositionLimits(Qty max_net_qty, Price max_notional) noexcept
    : max_net_qty_{max_net_qty}
    , max_notional_{max_notional}
{
    // slots_ 中每个 std::atomic<Qty> 已在成员初始化时值初始化为 0
}

bool PositionLimits::check_and_reserve(
    InstrumentId id, Side side, Qty qty, Price price) noexcept
{
    // 越界保护
    if (id >= MAX_INSTRUMENTS) return false;

    // 1. 名义金额检查（price × qty <= max_notional_）
    //    全部使用整数乘法；price/qty 均为 int64_t，乘积可能溢出需注意范围
    //    实际部署时 price < 1e9, qty < 1e6，乘积 < 1e15 < INT64_MAX（9.2e18），安全
    if (price > 0 && qty > 0) {
        if (price * qty > max_notional_) return false;
    }

    // 2. CAS 更新净持仓，超过 ±max_net_qty_ 则拒绝
    std::atomic<Qty>& slot = slots_[id].v;
    Qty delta = (side == Side::BUY) ? qty : -qty;

    Qty old_qty = slot.load(std::memory_order_relaxed);
    Qty new_qty;
    do {
        new_qty = old_qty + delta;
        // 超出持仓限额则拒绝
        if (new_qty > max_net_qty_ || new_qty < -max_net_qty_) {
            return false;
        }
    } while (!slot.compare_exchange_weak(
        old_qty, new_qty,
        std::memory_order_acq_rel,
        std::memory_order_relaxed));

    return true;
}

void PositionLimits::on_fill(
    InstrumentId id, Side side, Qty fill_qty) noexcept
{
    // check_and_reserve 在报单时已原子更新了持仓，
    // on_fill 当前实现为空（留作未来"预留 vs 实际差值修正"扩展点）
    (void)id; (void)side; (void)fill_qty;
}

void PositionLimits::on_cancel(
    InstrumentId id, Side side, Qty qty) noexcept
{
    // 撤单：归还 check_and_reserve 预留的额度（反向操作）
    if (id >= MAX_INSTRUMENTS) return;

    std::atomic<Qty>& slot = slots_[id].v;
    Qty delta = (side == Side::BUY) ? -qty : qty;  // 与 reserve 时方向相反

    // fetch_add 即可，无需检查边界（撤单只是归还，不会越界）
    slot.fetch_add(delta, std::memory_order_relaxed);
}

Qty PositionLimits::get_net_qty(InstrumentId id) const noexcept {
    if (id >= MAX_INSTRUMENTS) return 0;
    return slots_[id].v.load(std::memory_order_relaxed);
}

} // namespace hft
