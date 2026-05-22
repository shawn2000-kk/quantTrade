#include "strategy/stat_arb.hpp"
#include <cmath>
#include <utility>

namespace hft {

// ── Welford 在线统计 ─────────────────────────────────────────────────────
// 固定窗口近似：count 超过 window 后用 EMA 衰减均值和方差
// alpha = 2 / (window + 1)

void StatArb::WelfordStat::update(double x) noexcept {
    if (count < window) {
        // Welford 精确版（前 window 个样本）
        ++count;
        const double delta = x - mean;
        mean += delta / static_cast<double>(count);
        const double delta2 = x - mean;
        m2   += delta * delta2;
    } else {
        // 指数衰减近似：α = 2/(window+1)
        const double alpha = 2.0 / (static_cast<double>(window) + 1.0);
        const double delta = x - mean;
        mean = mean + alpha * delta;
        m2   = (1.0 - alpha) * (m2 + alpha * delta * delta);
    }
}

double StatArb::WelfordStat::variance() const noexcept {
    if (count < 2) return 1.0;  // 样本不足，返回 1 避免除零
    if (count < window) return m2 / static_cast<double>(count - 1);
    return m2;  // EMA 阶段 m2 直接是方差估计
}

double StatArb::WelfordStat::stddev() const noexcept {
    const double v = variance();
    return (v > 0.0) ? std::sqrt(v) : 1.0;
}

// ── StatArb 构造 ─────────────────────────────────────────────────────────

StatArb::StatArb(InstrumentId instrument_a,
                 InstrumentId instrument_b,
                 double       hedge_ratio,
                 Qty          order_qty,
                 double       entry_z,
                 double       exit_z,
                 double       stop_z,
                 size_t       lookback) noexcept
    : instrument_b_(instrument_b)
    , hedge_ratio_(hedge_ratio)
    , order_qty_(order_qty)
    , entry_z_(entry_z)
    , exit_z_(exit_z)
    , stop_z_(stop_z)
    , stat_{0.0, 0.0, 0, lookback}
{
    instrument_id_ = instrument_a;
}

// ── on_bbo_impl ──────────────────────────────────────────────────────────
// 收到 BBO 更新后缓存 mid price，两腿都有数据后计算价差

void StatArb::on_bbo_impl(const BBOEvent& e) noexcept {
    if (!e.is_valid_bbo()) return;

    const Price mid = e.mid_price();

    if (e.instrument_id == instrument_id_) {
        last_mid_a_ = mid;
    } else if (e.instrument_id == instrument_b_) {
        last_mid_b_ = mid;
    } else {
        return;
    }

    // 两腿都有有效数据才计算
    if (last_mid_a_ == 0 || last_mid_b_ == 0) return;

    // 价差（以整数 tick 为单位）
    const double spread = static_cast<double>(last_mid_a_)
                        - hedge_ratio_ * static_cast<double>(last_mid_b_);

    stat_.update(spread);

    // 样本不足 1/4 窗口时不交易（统计不稳定）
    if (stat_.count < stat_.window / 4) return;

    current_z_ = (spread - stat_.mean) / stat_.stddev();

    if (state_ == ArbState::FLAT) {
        try_open();
    } else {
        try_close();
    }
}

void StatArb::try_open() noexcept {
    if (current_z_ > entry_z_) {
        // 价差偏高：卖 A 买 B（Short Spread）
        send_pair(Side::SELL, Side::BUY);
        state_ = ArbState::SHORT_SPREAD;
    } else if (current_z_ < -entry_z_) {
        // 价差偏低：买 A 卖 B（Long Spread）
        send_pair(Side::BUY, Side::SELL);
        state_ = ArbState::LONG_SPREAD;
    }
}

void StatArb::try_close() noexcept {
    const double az = current_z_ < 0 ? -current_z_ : current_z_;

    const bool revert = az < exit_z_;
    const bool stop   = az > stop_z_;

    if (!revert && !stop) return;

    // 反向平仓
    if (state_ == ArbState::LONG_SPREAD) {
        send_pair(Side::SELL, Side::BUY);   // 平多 A，平空 B
    } else {
        send_pair(Side::BUY, Side::SELL);   // 平空 A，平多 B
    }
    state_ = ArbState::FLAT;
}

void StatArb::send_pair(Side side_a, Side side_b) noexcept {
    OrderRequest req_a{};
    req_a.instrument_id = instrument_id_;
    req_a.side          = side_a;
    req_a.type          = OrderType::IOC;
    req_a.price         = last_mid_a_;
    req_a.qty           = order_qty_;

    OrderRequest req_b{};
    req_b.instrument_id = instrument_b_;
    req_b.side          = side_b;
    req_b.type          = OrderType::IOC;
    req_b.price         = last_mid_b_;
    req_b.qty           = order_qty_;

    send_order(std::move(req_a));
    send_order(std::move(req_b));
}

// ── on_fill_impl ─────────────────────────────────────────────────────────

void StatArb::on_fill_impl(const FillEvent& f) noexcept {
    const Qty delta = (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    if (f.instrument_id == instrument_id_) {
        inventory_a_.fetch_add(delta, std::memory_order_relaxed);
    } else if (f.instrument_id == instrument_b_) {
        inventory_b_.fetch_add(delta, std::memory_order_relaxed);
    }
}

void StatArb::on_order_ack_impl(const OrderAck& /*a*/) noexcept {
    // 统计套利不依赖 ACK 做二次决策
}

// ── 诊断接口 ─────────────────────────────────────────────────────────────

double StatArb::current_zscore() const noexcept { return current_z_; }

int StatArb::position_state() const noexcept {
    switch (state_) {
        case ArbState::LONG_SPREAD:  return +1;
        case ArbState::SHORT_SPREAD: return -1;
        default:                     return  0;
    }
}

}  // namespace hft
