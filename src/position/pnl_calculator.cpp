#include "position/pnl_calculator.hpp"
#include <algorithm>  // std::min

namespace hft {

Price PnlCalculator::calc_unrealized_pnl(InstrumentId id,
                                          Price        mark_price) const noexcept {
    if (id >= MAX_INSTRUMENTS) [[unlikely]] return 0;

    // acquire net_qty 确保可见 avg_cost 的最新写入（参见 on_fill release 语义）
    const Qty   net_qty  = pm_.slot(id).net_qty.load(std::memory_order_acquire);
    const Price avg_cost = pm_.slot(id).avg_cost.load(std::memory_order_relaxed);

    if (net_qty == 0) return 0;

    // 公式：(mark_price - avg_cost) * net_qty
    // 多空均正确：空仓 net_qty < 0，价格下跌时 (mark < avg_cost)，结果为正（盈利）
    return (mark_price - avg_cost) * net_qty;
}

Price PnlCalculator::calc_total_unrealized_pnl(const Price mark_prices[],
                                                size_t      n) const noexcept {
    const size_t limit = (n <= MAX_INSTRUMENTS) ? n : MAX_INSTRUMENTS;
    Price total = 0;
    for (size_t i = 0; i < limit; ++i) {
        total += calc_unrealized_pnl(static_cast<InstrumentId>(i), mark_prices[i]);
    }
    return total;
}

Price PnlCalculator::calc_total_pnl(const Price mark_prices[],
                                     size_t      n) const noexcept {
    return pm_.get_total_realized_pnl()
         + calc_total_unrealized_pnl(mark_prices, n);
}

} // namespace hft
