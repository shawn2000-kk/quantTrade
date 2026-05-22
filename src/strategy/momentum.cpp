#include "strategy/momentum.hpp"
#include <cmath>
#include <utility>

namespace hft {

// ── 构造 ─────────────────────────────────────────────────────────────────

Momentum::Momentum(InstrumentId instrument_id,
                   int          fast_period,
                   int          slow_period,
                   Qty          order_qty,
                   Price        signal_band,
                   Price        max_adverse) noexcept
    : fast_alpha_(2.0 / (fast_period + 1))
    , slow_alpha_(2.0 / (slow_period + 1))
    , warmup_needed_(slow_period)
    , order_qty_(order_qty)
    , signal_band_(signal_band)
    , max_adverse_(max_adverse)
{
    instrument_id_ = instrument_id;
}

// ── EMA 辅助 ─────────────────────────────────────────────────────────────

double Momentum::ema_update(double prev, double price, double alpha) noexcept {
    return prev + alpha * (price - prev);
}

void Momentum::update_ema(double mid) noexcept {
    if (warmup_count_ == 0) {
        fast_ema_ = mid;
        slow_ema_ = mid;
    } else {
        fast_ema_ = ema_update(fast_ema_, mid, fast_alpha_);
        slow_ema_ = ema_update(slow_ema_, mid, slow_alpha_);
    }
    if (warmup_count_ < warmup_needed_) ++warmup_count_;
}

// ── 信号计算 ─────────────────────────────────────────────────────────────

Momentum::Signal Momentum::compute_signal() const noexcept {
    if (warmup_count_ < warmup_needed_) return Signal::NONE;

    const double diff = fast_ema_ - slow_ema_;

    if (diff > static_cast<double>(signal_band_)) return Signal::LONG;
    if (diff < -static_cast<double>(signal_band_)) return Signal::SHORT;
    return Signal::NONE;
}

// ── 下单辅助 ─────────────────────────────────────────────────────────────

void Momentum::open_position(Signal sig) noexcept {
    OrderRequest req{};
    req.instrument_id = instrument_id_;
    req.type          = OrderType::IOC;   // 立即成交否则撤单
    req.qty           = order_qty_;

    if (sig == Signal::LONG) {
        req.side  = Side::BUY;
        req.price = static_cast<Price>(fast_ema_) + 1;  // 略微追价
    } else {
        req.side  = Side::SELL;
        req.price = static_cast<Price>(fast_ema_) - 1;
    }

    entry_price_   = static_cast<Price>(fast_ema_);
    last_signal_   = sig;
    send_order(std::move(req));
}

void Momentum::close_position() noexcept {
    const Qty inv = inventory_.load(std::memory_order_relaxed);
    if (inv == 0) {
        last_signal_ = Signal::NONE;
        entry_price_ = 0;
        return;
    }

    OrderRequest req{};
    req.instrument_id = instrument_id_;
    req.type          = OrderType::IOC;

    if (inv > 0) {
        req.side  = Side::SELL;
        req.qty   = inv;
        req.price = static_cast<Price>(fast_ema_) - 1;
    } else {
        req.side  = Side::BUY;
        req.qty   = -inv;
        req.price = static_cast<Price>(fast_ema_) + 1;
    }

    last_signal_ = Signal::NONE;
    entry_price_ = 0;
    send_order(std::move(req));
}

void Momentum::check_stop_loss(Price mid) noexcept {
    if (entry_price_ == 0) return;
    const Qty inv = inventory_.load(std::memory_order_relaxed);
    if (inv == 0) return;

    const Price adverse = (inv > 0)
        ? (entry_price_ - mid)    // 多头时价格下跌
        : (mid - entry_price_);   // 空头时价格上涨

    if (adverse > max_adverse_) {
        close_position();
    }
}

// ── on_bbo_impl ──────────────────────────────────────────────────────────

void Momentum::on_bbo_impl(const BBOEvent& e) noexcept {
    if (e.instrument_id != instrument_id_) return;
    if (!e.is_valid_bbo()) return;

    const double mid = static_cast<double>(e.mid_price());
    update_ema(mid);

    const Price mid_px = e.mid_price();

    // 止损检查（优先于信号）
    check_stop_loss(mid_px);

    const Signal sig = compute_signal();
    const Qty    inv = inventory_.load(std::memory_order_relaxed);

    if (inv == 0) {
        // 空仓：检查是否开仓
        if (sig != Signal::NONE && sig != last_signal_) {
            open_position(sig);
        }
    } else {
        // 持仓：检查是否需要翻转方向或平仓
        const bool long_pos  = (inv > 0);
        const bool flip_long = (sig == Signal::SHORT && long_pos);
        const bool flip_short= (sig == Signal::LONG  && !long_pos);
        const bool flat      = (sig == Signal::NONE);

        if (flat || flip_long || flip_short) {
            close_position();
            // 翻转时下一次 BBO 再开仓（避免当 tick 同时发两笔 IOC）
        }
    }
}

// ── on_fill_impl ─────────────────────────────────────────────────────────

void Momentum::on_fill_impl(const FillEvent& f) noexcept {
    if (f.instrument_id != instrument_id_) return;
    const Qty delta = (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    inventory_.fetch_add(delta, std::memory_order_relaxed);
}

void Momentum::on_order_ack_impl(const OrderAck& /*a*/) noexcept {
    // IOC 报单无需跟踪 ACK
}

}  // namespace hft
