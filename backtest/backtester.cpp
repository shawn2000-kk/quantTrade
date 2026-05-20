#include "backtest/backtester.hpp"

namespace hft {

Backtester::Backtester(SimExchange& exchange, PositionManager& pm) noexcept
    : exchange_(exchange), pm_(pm)
{}

void Backtester::set_ack_handler(std::function<void(const OrderAck&)> handler) noexcept {
    ack_handler_ = static_cast<decltype(handler)&&>(handler);
}

void Backtester::set_fill_handler(std::function<void(const FillEvent&)> handler) noexcept {
    fill_handler_ = static_cast<decltype(handler)&&>(handler);
}

void Backtester::feed_bbo(const BBOEvent& bbo) noexcept {
    ++bbo_count_;

    // 1. 驱动模拟交易所撮合
    exchange_.process_bbo(bbo);

    // 2. 消费所有 ACK
    {
        OrderAck ack{};
        while (exchange_.pop_ack(ack)) {
            if (ack_handler_) {
                ack_handler_(ack);
            }
        }
    }

    // 3. 消费所有成交回报
    {
        FillEvent fill{};
        while (exchange_.pop_fill(fill)) {
            ++fill_count_;
            // 更新持仓
            pm_.on_fill(fill);
            // 通知策略层
            if (fill_handler_) {
                fill_handler_(fill);
            }
        }
    }
}

} // namespace hft
