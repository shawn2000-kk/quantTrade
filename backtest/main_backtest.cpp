#include <cmath>
#include <cstdio>
#include <cstdint>

#include "backtest/backtester.hpp"
#include "backtest/metrics_report.hpp"
#include "backtest/sim_exchange.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"
#include "position/position_manager.hpp"

// ============================================================
// main_backtest.cpp — 回测框架端到端演示
//
// 合成行情：1000 条 BBOEvent，价格随正弦波变化
//   base_price = 1000 tick
//   amplitude  = 50  tick
//   price      = base + (int64_t)(amplitude * sin(i * 2π / 100))
//   bid_px     = price - 1
//   ask_px     = price + 1
//
// 策略：
//   - 每 10 条 BBO 发一笔限价买单（qty=1，price=ask_px → 立即成交）
//   - 偏移 5 条后每 10 条发一笔限价卖单（qty=1，price=bid_px → 立即成交）
//
// 绩效：每 100 条 BBO 视为一个交易日，记录当日已实现 P&L
// ============================================================

static constexpr int64_t  BASE_PRICE    = 1000;
static constexpr int64_t  AMPLITUDE     = 50;
static constexpr int      NUM_BBO       = 1000;
static constexpr int      PERIOD        = 100;   // 正弦波周期（条）
static constexpr int      BBOS_PER_DAY  = 100;   // 每个"交易日"的 BBO 数
static constexpr uint32_t INSTRUMENT_ID = 0;

namespace hft {
namespace {

BBOEvent make_bbo(int idx) noexcept {
    const double angle    = static_cast<double>(idx) * 2.0 * 3.14159265358979 / static_cast<double>(PERIOD);
    const int64_t mid     = BASE_PRICE + static_cast<int64_t>(static_cast<double>(AMPLITUDE) * std::sin(angle));
    BBOEvent bbo{};
    bbo.exchange_ts_ns  = static_cast<uint64_t>(idx) * 1'000'000;  // 每条间隔 1ms
    bbo.local_ts_ns     = bbo.exchange_ts_ns;
    bbo.instrument_id   = INSTRUMENT_ID;
    bbo.type            = EventType::BBO_UPDATE;
    bbo.bid_px          = mid - 1;
    bbo.ask_px          = mid + 1;
    bbo.bid_qty         = 100;
    bbo.ask_qty         = 100;
    return bbo;
}

} // anonymous namespace
} // namespace hft

int main() {
    using namespace hft;

    // 构建组件
    SimExchange     exchange;            // 默认：无滑点，全成交
    PositionManager pm;
    Backtester      backtester(exchange, pm);
    MetricsReport   report;

    // 统计成交次数（通过 fill_handler 计数）
    uint64_t fill_count = 0;
    backtester.set_fill_handler([&fill_count](const FillEvent& /*f*/) noexcept {
        ++fill_count;
    });

    int64_t prev_realized_pnl = 0;

    for (int i = 0; i < NUM_BBO; ++i) {
        // 生成当前 BBO
        const BBOEvent bbo = make_bbo(i);

        // ── 策略：每 10 条发一笔买单，偏移 5 条后每 10 条发一笔卖单 ──
        if (i % 10 == 0) {
            // 限价买单，报价 = ask_px（立即可成交）
            OrderRequest req{};
            req.instrument_id  = INSTRUMENT_ID;
            req.side           = Side::BUY;
            req.type           = OrderType::LIMIT;
            req.price          = bbo.ask_px;
            req.qty            = 1;
            req.strategy_ts_ns = bbo.local_ts_ns;
            exchange.send_new_order(req);
        }
        if (i % 10 == 5) {
            // 限价卖单，报价 = bid_px（立即可成交）
            OrderRequest req{};
            req.instrument_id  = INSTRUMENT_ID;
            req.side           = Side::SELL;
            req.type           = OrderType::LIMIT;
            req.price          = bbo.bid_px;
            req.qty            = 1;
            req.strategy_ts_ns = bbo.local_ts_ns;
            exchange.send_new_order(req);
        }

        // ── 回测引擎驱动：撮合 + 持仓更新 ──
        backtester.feed_bbo(bbo);

        // ── 每个"交易日"结束，记录当日已实现 P&L ──
        if ((i + 1) % BBOS_PER_DAY == 0) {
            const int64_t cur_realized = pm.get_total_realized_pnl();
            report.record_pnl(cur_realized - prev_realized_pnl);
            prev_realized_pnl = cur_realized;
        }
    }

    // ── 打印回测结果 ──────────────────────────────────────────────
    std::printf("=== Backtest Results ===\n");
    std::printf("total_fills    : %llu\n",  static_cast<unsigned long long>(backtester.total_fills()));
    std::printf("trading_days   : %zu\n",   report.trading_days());
    std::printf("total_pnl      : %lld\n",  static_cast<long long>(report.total_pnl()));
    std::printf("max_daily_pnl  : %lld\n",  static_cast<long long>(report.max_daily_pnl()));
    std::printf("min_daily_pnl  : %lld\n",  static_cast<long long>(report.min_daily_pnl()));
    std::printf("sharpe_ratio   : %.4f\n",  report.sharpe_ratio());
    std::printf("max_drawdown   : %.4f\n",  report.max_drawdown());
    std::printf("win_rate       : %.4f\n",  report.win_rate());

    return 0;
}
