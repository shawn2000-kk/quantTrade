#include "strategy/market_maker.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"
#include <utility>   // std::move

namespace hft {

MarketMaker::MarketMaker(InstrumentId instrument_id,
                         Price        spread_ticks,
                         Qty          order_qty,
                         Qty          max_inventory) noexcept
    : spread_ticks_(spread_ticks)
    , order_qty_(order_qty)
    , max_inventory_(max_inventory)
{
    instrument_id_ = instrument_id;
}

// ── on_bbo_impl ─────────────────────────────────────────────────────────
//
// 热路径调用（策略线程，核心 3）
// 1. 过滤非本品种 / 无效 BBO
// 2. 检测 mid 是否移动 >= 0.5 tick（用 bid+ask 之和，精度 0.5 tick）
// 3. 撤旧单、重新报价（含库存偏斜和库存超限检查）

void MarketMaker::on_bbo_impl(const BBOEvent& e) noexcept {
    // 过滤：只处理本品种的有效 BBO
    if (e.instrument_id != instrument_id_) return;
    if (!e.is_valid_bbo()) return;

    // 用 bid_px + ask_px 作为 "2x mid"，精度 0.5 tick
    const Price new_sum = e.bid_px + e.ask_px;
    const Price old_sum = last_mid_.load(std::memory_order_relaxed);

    // 计算绝对变化量（手动 abs，避免 <cmath> 依赖）
    const Price delta = (new_sum > old_sum) ? (new_sum - old_sum)
                                            : (old_sum - new_sum);

    // old_sum == 0 表示首次报价，无论如何都要报；
    // delta == 0 表示 mid 完全未变，跳过（不必要撤单重报）
    if (old_sum != 0 && delta == 0) return;

    // 持久化新 mid（写入 relaxed，仅策略线程读写此值）
    last_mid_.store(new_sum, std::memory_order_relaxed);

    // 撤销现有挂单（若有），OMS 线程通过 pop_cancel() 消费
    cancel_active_orders();

    // 报价计算
    // 使用整数 mid（截断），spread 对半分（向下取整 + 余量给 ask）
    const Price mid         = new_sum / 2;  // 整数 mid（截断 0.5 tick）
    const Price half_spread = spread_ticks_ / 2;
    Price bid_price = mid - half_spread;
    Price ask_price = mid + (spread_ticks_ - half_spread);

    // 库存偏斜：多头降低 ask（加速卖出），空头提高 bid（加速买入）
    const Qty inv = inventory_.load(std::memory_order_relaxed);
    if (inv > 0) ask_price -= 1;
    if (inv < 0) bid_price += 1;

    quote(bid_price, ask_price, inv);
}

// ── on_fill_impl ────────────────────────────────────────────────────────
//
// OMS/持仓线程调用，仅更新 inventory_（atomic CAS-free add）

void MarketMaker::on_fill_impl(const FillEvent& f) noexcept {
    if (f.instrument_id != instrument_id_) return;
    const Qty delta = (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    inventory_.fetch_add(delta, std::memory_order_relaxed);
}

// ── on_order_ack_impl ────────────────────────────────────────────────────
//
// 策略线程调用（ACK 经 SPSC 传回）
// 从 pending_side_queue_ 弹出发单时记录的方向，关联 client_order_id

void MarketMaker::on_order_ack_impl(const OrderAck& a) noexcept {
    if (a.status == OrderStatus::REJECTED) {
        // 拒绝：丢弃 pending_side_queue_ 中对应的条目，清零对应 ID
        Side s;
        if (pending_side_queue_.pop(s)) {
            if (s == Side::BUY)  bid_order_id_ = 0;
            else                 ask_order_id_ = 0;
        }
        return;
    }

    // NEW ACK：记录 client_order_id 以便后续撤单
    Side s;
    if (pending_side_queue_.pop(s)) {
        if (s == Side::BUY)  bid_order_id_ = a.client_order_id;
        else                 ask_order_id_ = a.client_order_id;
    }
}

// ── 私有辅助：cancel_active_orders ──────────────────────────────────────

void MarketMaker::cancel_active_orders() noexcept {
    if (bid_order_id_ != 0) {
        cancel_queue_.push(bid_order_id_);
        bid_order_id_ = 0;
    }
    if (ask_order_id_ != 0) {
        cancel_queue_.push(ask_order_id_);
        ask_order_id_ = 0;
    }
}

// ── 私有辅助：quote ──────────────────────────────────────────────────────
//
// 根据库存限制决定是否报双边，推入 order_queue_ 和 pending_side_queue_

void MarketMaker::quote(Price bid_price, Price ask_price, Qty inv) noexcept {
    // 多头未超限时报 bid（允许继续买入）
    if (inv < max_inventory_) {
        OrderRequest bid{};
        bid.instrument_id = instrument_id_;
        bid.side          = Side::BUY;
        bid.type          = OrderType::LIMIT;
        bid.price         = bid_price;
        bid.qty           = order_qty_;
        // strategy_ts_ns 留 0（可由调用方注入 RDTSC，此处精简）
        if (send_order(std::move(bid))) {
            pending_side_queue_.push(Side::BUY);
        }
    }

    // 空头未超限时报 ask（允许继续卖出）
    if (inv > -max_inventory_) {
        OrderRequest ask{};
        ask.instrument_id = instrument_id_;
        ask.side          = Side::SELL;
        ask.type          = OrderType::LIMIT;
        ask.price         = ask_price;
        ask.qty           = order_qty_;
        if (send_order(std::move(ask))) {
            pending_side_queue_.push(Side::SELL);
        }
    }
}

} // namespace hft
