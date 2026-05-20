// src/execution/pov.cpp

#include "execution/pov.hpp"

#include <algorithm>   // std::min

namespace hft {

Pov::Pov(IGateway&    gw,
         InstrumentId instrument_id,
         Side         side,
         Qty          total_qty,
         double       participation_rate) noexcept
    : gw_                (gw)
    , instrument_id_     (instrument_id)
    , side_              (side)
    , total_qty_         (total_qty)
    , participation_rate_(participation_rate)
{}

void Pov::on_market_trade(Qty market_trade_qty) noexcept {
    market_volume_ += market_trade_qty;

    // 参与目标量 = 累积市场成交量 × 参与率
    const Qty target = static_cast<Qty>(
        static_cast<double>(market_volume_) * participation_rate_
    );

    // 尚未提交到 Gateway 的缺口
    const Qty gap = target - ordered_qty_;
    if (gap <= kMinOrderSize) return;

    // 限制在剩余总量内
    const Qty order_qty = std::min(gap, total_qty_ - ordered_qty_);
    if (order_qty <= 0) return;

    OrderRequest req{};
    req.instrument_id  = instrument_id_;
    req.side           = side_;
    req.type           = OrderType::MARKET;
    req.price          = 0;
    req.qty            = order_qty;
    req.strategy_ts_ns = 0;  // 热路径不打时间戳，减少 rdtsc 开销
    gw_.send_new_order(req);
    ordered_qty_ += order_qty;
}

void Pov::on_fill(Qty fill_qty) noexcept {
    sent_qty_ += fill_qty;
}

bool Pov::is_done() const noexcept {
    return sent_qty_ >= total_qty_;
}

} // namespace hft
