#include "backtest/sim_exchange.hpp"

namespace hft {

SimExchange::SimExchange(double slippage_ticks, double fill_ratio) noexcept
    : slippage_ticks_(slippage_ticks), fill_ratio_(fill_ratio)
{}

void SimExchange::send_new_order(const OrderRequest& req) noexcept {
    PendingEntry entry{};
    entry.req                 = req;
    entry.arrive_ts_ns        = req.strategy_ts_ns;
    entry.client_order_id     = next_client_order_id_++;
    entry.exchange_order_id   = next_exchange_order_id_++;
    pending_orders_.push(entry);
}

void SimExchange::send_cancel(uint64_t /*client_order_id*/) noexcept {}
void SimExchange::send_modify(const ModifyRequest& /*req*/)  noexcept {}

void SimExchange::process_bbo(const BBOEvent& bbo) noexcept {
    if (!bbo.is_valid_bbo()) return;

    // 将滑点转换为整数 tick（向下取整，保守估计可成交范围）
    const auto slippage = static_cast<Price>(slippage_ticks_);

    const size_t n = pending_orders_.size();
    for (size_t i = 0; i < n; ++i) {
        PendingEntry entry = pending_orders_.front();
        pending_orders_.pop();

        const OrderRequest& req = entry.req;
        bool can_fill = false;

        if (req.type == OrderType::LIMIT) {
            if (req.side == Side::BUY) {
                // 买单成交条件：ask <= order.price + slippage（愿意接受 ask 在上方 slippage 以内）
                can_fill = (bbo.ask_px <= req.price + slippage);
            } else {
                // 卖单成交条件：bid >= order.price - slippage（愿意接受 bid 在下方 slippage 以内）
                can_fill = (bbo.bid_px >= req.price - slippage);
            }
        } else if (req.type == OrderType::MARKET) {
            can_fill = true; // 市价单始终成交
        }

        if (!can_fill) {
            // 条件不满足，放回队列等待下次撮合
            pending_orders_.push(entry);
            continue;
        }

        // 按 fill_ratio 计算成交数量
        const Qty fill_qty = static_cast<Qty>(static_cast<double>(req.qty) * fill_ratio_);
        if (fill_qty <= 0) {
            // fill_ratio = 0，不成交，直接丢弃（不放回队列）
            continue;
        }

        // 成交价：买单取 ask，卖单取 bid
        const Price fill_price = (req.side == Side::BUY) ? bbo.ask_px : bbo.bid_px;
        const Qty   remaining  = req.qty - fill_qty;

        // 生成 OrderAck（NEW → FILLED 简化，直接报 NEW）
        OrderAck ack{};
        ack.client_order_id   = entry.client_order_id;
        ack.exchange_order_id = entry.exchange_order_id;
        ack.status            = OrderStatus::NEW;
        ack.ack_ts_ns         = bbo.local_ts_ns;
        ack_queue_.push(ack);

        // 生成 FillEvent
        FillEvent fill{};
        fill.client_order_id   = entry.client_order_id;
        fill.exchange_order_id = entry.exchange_order_id;
        fill.instrument_id     = req.instrument_id;
        fill.side              = req.side;
        fill.fill_price        = fill_price;
        fill.fill_qty          = fill_qty;
        fill.remaining_qty     = remaining;
        fill.fill_ts_ns        = bbo.local_ts_ns;
        fill_queue_.push(fill);

        // 若有剩余且 fill_ratio < 1.0，将剩余部分放回队列（部分成交）
        if (remaining > 0) {
            PendingEntry leftover = entry;
            leftover.req.qty = remaining;
            pending_orders_.push(leftover);
        }
    }
}

bool SimExchange::pop_ack(OrderAck& out) noexcept {
    if (ack_queue_.empty()) return false;
    out = ack_queue_.front();
    ack_queue_.pop();
    return true;
}

bool SimExchange::pop_fill(FillEvent& out) noexcept {
    if (fill_queue_.empty()) return false;
    out = fill_queue_.front();
    fill_queue_.pop();
    return true;
}

} // namespace hft
