#include "position/position_manager.hpp"
#include <algorithm>  // std::min

namespace hft {

// ── on_fill ───────────────────────────────────────────────────────────────
//
// 内存顺序说明：
//   avg_cost / realized_pnl 的更新先于 net_qty 写入（release）。
//   读者通过 acquire 读取 net_qty 后，可见最新的 avg_cost / realized_pnl。
//   在单 PositionThread 写入的前提下，relaxed 中间操作是安全的。
//
// 价格/数量均为正整数（tick 单位），Side::BUY / SELL 区分方向。
//
void PositionManager::on_fill(const FillEvent& fill) noexcept {
    if (fill.instrument_id >= MAX_INSTRUMENTS) [[unlikely]] return;

    Slot& slot = slots_[fill.instrument_id];

    // 读取当前快照（单 PositionThread 写入，此处 relaxed 安全）
    const Qty   cur_qty  = slot.net_qty.load(std::memory_order_relaxed);
    const Price cur_avg  = slot.avg_cost.load(std::memory_order_relaxed);
    const Qty   fill_qty = fill.fill_qty;
    const Price fill_px  = fill.fill_price;

    if (fill.side == Side::BUY) {
        // ── BUY 成交 ──────────────────────────────────────────────────────
        const Qty new_qty = cur_qty + fill_qty;

        if (cur_qty >= 0) {
            // 场景 A：持有多仓或空仓（flat → 开多）
            // 加权均价：new_avg = (cur_avg * cur_qty + fill_px * fill_qty) / new_qty
            Price new_avg = 0;
            if (new_qty > 0) {
                new_avg = (cur_avg * cur_qty + fill_px * fill_qty) / new_qty;
            }
            slot.avg_cost.store(new_avg, std::memory_order_relaxed);
        } else {
            // 场景 B：持有空仓，BUY 是平仓方向
            const Qty abs_short = -cur_qty;                  // 当前空仓绝对量
            const Qty close_qty = (fill_qty <= abs_short)    // 本次平仓量
                                ? fill_qty
                                : abs_short;

            // 平空头 realized PnL：空头盈利在价格下跌，公式：(avg_cost - fill_px) * close_qty
            const Price pnl_delta = (cur_avg - fill_px) * close_qty;
            slot.realized_pnl.fetch_add(pnl_delta, std::memory_order_relaxed);

            if (new_qty > 0) {
                // 翻仓：超额平空后开了多头，新均价 = fill_px
                slot.avg_cost.store(fill_px, std::memory_order_relaxed);
            } else if (new_qty == 0) {
                // 恰好全部平空，avg_cost 归零
                slot.avg_cost.store(static_cast<Price>(0), std::memory_order_relaxed);
            }
            // new_qty < 0：仍为空仓，avg_cost 不变（剩余仓位成本不变）
        }

        // net_qty release：对读者可见（同时使 avg_cost / realized_pnl 可见）
        slot.net_qty.store(new_qty, std::memory_order_release);

    } else {
        // ── SELL 成交 ─────────────────────────────────────────────────────
        const Qty new_qty = cur_qty - fill_qty;

        if (cur_qty <= 0) {
            // 场景 C：持有空仓或 flat（flat → 开空）
            // 加权均价（注意：new_qty 为负，abs = -new_qty）
            Price new_avg = 0;
            if (new_qty < 0) {
                const Qty abs_new = -new_qty;
                const Qty abs_cur = -cur_qty;
                new_avg = (cur_avg * abs_cur + fill_px * fill_qty) / abs_new;
            }
            slot.avg_cost.store(new_avg, std::memory_order_relaxed);
        } else {
            // 场景 D：持有多仓，SELL 是平仓方向
            const Qty close_qty = (fill_qty <= cur_qty) ? fill_qty : cur_qty;

            // 平多头 realized PnL：(fill_px - avg_cost) * close_qty
            const Price pnl_delta = (fill_px - cur_avg) * close_qty;
            slot.realized_pnl.fetch_add(pnl_delta, std::memory_order_relaxed);

            if (new_qty < 0) {
                // 翻仓：超额平多后开了空头，新均价 = fill_px
                slot.avg_cost.store(fill_px, std::memory_order_relaxed);
            } else if (new_qty == 0) {
                // 恰好全部平多，avg_cost 归零
                slot.avg_cost.store(static_cast<Price>(0), std::memory_order_relaxed);
            }
            // new_qty > 0：仍为多仓，avg_cost 不变
        }

        slot.net_qty.store(new_qty, std::memory_order_release);
    }
}

// ── 读接口 ─────────────────────────────────────────────────────────────────

Qty PositionManager::get_net_qty(InstrumentId id) const noexcept {
    if (id >= MAX_INSTRUMENTS) [[unlikely]] return 0;
    return slots_[id].net_qty.load(std::memory_order_acquire);
}

Price PositionManager::get_avg_cost(InstrumentId id) const noexcept {
    if (id >= MAX_INSTRUMENTS) [[unlikely]] return 0;
    return slots_[id].avg_cost.load(std::memory_order_relaxed);
}

Price PositionManager::get_realized_pnl(InstrumentId id) const noexcept {
    if (id >= MAX_INSTRUMENTS) [[unlikely]] return 0;
    return slots_[id].realized_pnl.load(std::memory_order_relaxed);
}

Price PositionManager::get_total_realized_pnl() const noexcept {
    Price total = 0;
    for (size_t i = 0; i < MAX_INSTRUMENTS; ++i) {
        total += slots_[i].realized_pnl.load(std::memory_order_relaxed);
    }
    return total;
}

} // namespace hft
