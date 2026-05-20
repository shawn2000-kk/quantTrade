// tests/unit/test_backtest.cpp
// 回测框架单元测试
//
// 覆盖场景：
//   SimExchange:
//     1. send_new_order + process_bbo 后 pop_fill 有成交
//     2. slippage > 0 时部分价格不成交（ask 高于 order.price + slippage）
//     3. fill_ratio = 0.0 无成交
//   Backtester:
//     4. feed_bbo 触发 fill_handler 回调
//   MetricsReport:
//     5. sharpe_ratio 计算正确（已知序列验证）
//     6. max_drawdown 正确（峰谷验证）
//     7. win_rate 正确

#include <gtest/gtest.h>
#include <cmath>

#include "backtest/backtester.hpp"
#include "backtest/metrics_report.hpp"
#include "backtest/sim_exchange.hpp"
#include "common/types.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"
#include "position/position_manager.hpp"

namespace hft {
namespace {

// ── 辅助：构造 BBOEvent ───────────────────────────────────────────────────

BBOEvent make_bbo(Price bid, Price ask, uint64_t ts_ns = 1'000'000) noexcept {
    BBOEvent bbo{};
    bbo.exchange_ts_ns = ts_ns;
    bbo.local_ts_ns    = ts_ns;
    bbo.instrument_id  = 0;
    bbo.type           = EventType::BBO_UPDATE;
    bbo.bid_px         = bid;
    bbo.ask_px         = ask;
    bbo.bid_qty        = 100;
    bbo.ask_qty        = 100;
    return bbo;
}

// ── 辅助：构造限价 OrderRequest ──────────────────────────────────────────

OrderRequest make_limit_order(Side side, Price price, Qty qty = 10,
                               uint32_t instrument_id = 0) noexcept {
    OrderRequest req{};
    req.instrument_id  = instrument_id;
    req.side           = side;
    req.type           = OrderType::LIMIT;
    req.price          = price;
    req.qty            = qty;
    req.strategy_ts_ns = 0;
    return req;
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. SimExchange 基本撮合：send_new_order + process_bbo → pop_fill 有成交
// ═══════════════════════════════════════════════════════════════════════════

TEST(SimExchange, BasicFill_BuyLimit) {
    SimExchange ex(/*slippage=*/0.0, /*fill_ratio=*/1.0);

    // 买单报价 101，ask=100 → 100 <= 101+0 → 应成交
    ex.send_new_order(make_limit_order(Side::BUY, /*price=*/101, /*qty=*/5));

    const BBOEvent bbo = make_bbo(/*bid=*/99, /*ask=*/100);
    ex.process_bbo(bbo);

    FillEvent fill{};
    ASSERT_TRUE(ex.pop_fill(fill));
    EXPECT_EQ(fill.instrument_id, 0u);
    EXPECT_EQ(fill.side,          Side::BUY);
    EXPECT_EQ(fill.fill_qty,      5);
    EXPECT_EQ(fill.fill_price,    100);  // 成交价 = ask
    EXPECT_EQ(fill.remaining_qty, 0);

    // 队列已清空
    EXPECT_FALSE(ex.pop_fill(fill));
}

TEST(SimExchange, BasicFill_SellLimit) {
    SimExchange ex(0.0, 1.0);

    // 卖单报价 98，bid=99 → 99 >= 98-0 → 应成交
    ex.send_new_order(make_limit_order(Side::SELL, /*price=*/98, /*qty=*/3));

    const BBOEvent bbo = make_bbo(/*bid=*/99, /*ask=*/101);
    ex.process_bbo(bbo);

    FillEvent fill{};
    ASSERT_TRUE(ex.pop_fill(fill));
    EXPECT_EQ(fill.side,       Side::SELL);
    EXPECT_EQ(fill.fill_qty,   3);
    EXPECT_EQ(fill.fill_price, 99);  // 成交价 = bid
}

TEST(SimExchange, AckGenerated_OnFill) {
    SimExchange ex(0.0, 1.0);
    ex.send_new_order(make_limit_order(Side::BUY, 101));
    ex.process_bbo(make_bbo(99, 100));

    OrderAck ack{};
    ASSERT_TRUE(ex.pop_ack(ack));
    EXPECT_EQ(ack.status, OrderStatus::NEW);
    EXPECT_GT(ack.exchange_order_id, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. slippage > 0：ask 高于 order.price + slippage 时不成交
// ═══════════════════════════════════════════════════════════════════════════

TEST(SimExchange, SlippageBlocksFill) {
    // slippage = 1 tick
    SimExchange ex(/*slippage=*/1.0, /*fill_ratio=*/1.0);

    // 买单报价 100，ask=102 → 102 > 100+1=101 → 不应成交
    ex.send_new_order(make_limit_order(Side::BUY, /*price=*/100));
    ex.process_bbo(make_bbo(/*bid=*/100, /*ask=*/102));

    FillEvent fill{};
    EXPECT_FALSE(ex.pop_fill(fill));
    EXPECT_EQ(ex.pending_count(), 1u);  // 仍挂在队列中
}

TEST(SimExchange, SlippageAllowsFill) {
    // slippage = 2 tick
    SimExchange ex(/*slippage=*/2.0, /*fill_ratio=*/1.0);

    // 买单报价 100，ask=102 → 102 <= 100+2=102 → 应成交
    ex.send_new_order(make_limit_order(Side::BUY, /*price=*/100));
    ex.process_bbo(make_bbo(/*bid=*/98, /*ask=*/102));

    FillEvent fill{};
    EXPECT_TRUE(ex.pop_fill(fill));
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. fill_ratio = 0.0 → 无成交（订单被丢弃，不放回队列）
// ═══════════════════════════════════════════════════════════════════════════

TEST(SimExchange, FillRatioZero_NoFill) {
    SimExchange ex(0.0, /*fill_ratio=*/0.0);
    ex.send_new_order(make_limit_order(Side::BUY, 101, 10));
    ex.process_bbo(make_bbo(99, 100));

    FillEvent fill{};
    EXPECT_FALSE(ex.pop_fill(fill));
    // fill_ratio=0 时订单被丢弃，不放回队列
    EXPECT_EQ(ex.pending_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3b. fill_ratio = 0.5 → 部分成交
// ═══════════════════════════════════════════════════════════════════════════

TEST(SimExchange, FillRatioHalf_PartialFill) {
    SimExchange ex(0.0, /*fill_ratio=*/0.5);
    ex.send_new_order(make_limit_order(Side::BUY, 101, /*qty=*/10));
    ex.process_bbo(make_bbo(99, 100));

    FillEvent fill{};
    ASSERT_TRUE(ex.pop_fill(fill));
    EXPECT_EQ(fill.fill_qty,      5);   // 0.5 * 10
    EXPECT_EQ(fill.remaining_qty, 5);   // 剩余 5
    // 剩余部分放回队列
    EXPECT_EQ(ex.pending_count(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Backtester：feed_bbo 触发 fill_handler 回调
// ═══════════════════════════════════════════════════════════════════════════

TEST(Backtester, FillHandlerInvoked) {
    SimExchange     ex(0.0, 1.0);
    PositionManager pm;
    Backtester      bt(ex, pm);

    int fill_calls = 0;
    bt.set_fill_handler([&fill_calls](const FillEvent& /*f*/) noexcept {
        ++fill_calls;
    });

    // 发两笔买单
    ex.send_new_order(make_limit_order(Side::BUY, 101, 1));
    ex.send_new_order(make_limit_order(Side::BUY, 101, 1));

    bt.feed_bbo(make_bbo(99, 100));

    EXPECT_EQ(fill_calls,       2);
    EXPECT_EQ(bt.total_fills(), 2u);
    EXPECT_EQ(bt.processed_bbo_count(), 1u);
}

TEST(Backtester, AckHandlerInvoked) {
    SimExchange     ex(0.0, 1.0);
    PositionManager pm;
    Backtester      bt(ex, pm);

    int ack_calls = 0;
    bt.set_ack_handler([&ack_calls](const OrderAck& /*a*/) noexcept {
        ++ack_calls;
    });

    ex.send_new_order(make_limit_order(Side::BUY, 101, 1));
    bt.feed_bbo(make_bbo(99, 100));

    EXPECT_EQ(ack_calls, 1);
}

TEST(Backtester, FillUpdatesPositionManager) {
    SimExchange     ex(0.0, 1.0);
    PositionManager pm;
    Backtester      bt(ex, pm);

    // 买入 10 手 @ ask=100
    ex.send_new_order(make_limit_order(Side::BUY, 101, 10));
    bt.feed_bbo(make_bbo(99, 100));

    EXPECT_EQ(pm.get_net_qty(0), 10);
    EXPECT_EQ(pm.get_avg_cost(0), 100);
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. MetricsReport：sharpe_ratio 用已知序列验证
//    序列 [100, 100, 100]：均值=100，std_dev=0 → sharpe=0.0
//    序列 [100, -100]：均值=0 → sharpe=0.0
//    序列 [10, 20, 30]：均值=20，std_dev=10，sharpe=20/10*sqrt(252)
// ═══════════════════════════════════════════════════════════════════════════

TEST(MetricsReport, SharpeRatio_ZeroStdDev) {
    MetricsReport r;
    r.record_pnl(100);
    r.record_pnl(100);
    r.record_pnl(100);
    EXPECT_DOUBLE_EQ(r.sharpe_ratio(), 0.0);
}

TEST(MetricsReport, SharpeRatio_ZeroMean) {
    MetricsReport r;
    r.record_pnl(100);
    r.record_pnl(-100);
    EXPECT_DOUBLE_EQ(r.sharpe_ratio(), 0.0);
}

TEST(MetricsReport, SharpeRatio_KnownSequence) {
    // [10, 20, 30]: mean=20, sample_std = sqrt(((10-20)^2+(20-20)^2+(30-20)^2)/2)
    //             = sqrt((100+0+100)/2) = sqrt(100) = 10
    // sharpe = 20/10 * sqrt(252) = 2 * sqrt(252)
    MetricsReport r;
    r.record_pnl(10);
    r.record_pnl(20);
    r.record_pnl(30);

    const double expected = 2.0 * std::sqrt(252.0);
    EXPECT_NEAR(r.sharpe_ratio(), expected, 1e-9);
}

TEST(MetricsReport, SharpeRatio_SingleSample_ReturnsZero) {
    MetricsReport r;
    r.record_pnl(999);
    EXPECT_DOUBLE_EQ(r.sharpe_ratio(), 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. MetricsReport：max_drawdown 峰谷验证
//    序列 [100, -50, 200, -300]
//    累计：100, 50, 250, -50
//    峰值在第 3 条时为 250；之后跌到 -50
//    max_dd = (250 - (-50)) / 250 = 300/250 = 1.2 → 但 drawdown 最大为 1.0
//    实际：(250 - (-50)) / 250 = 1.2，clamp 到 1.0? 不，drawdown 可 > 1
//    按代码逻辑：dd = (peak - cumulative) / peak = (250 - (-50)) / 250 = 1.2
// ═══════════════════════════════════════════════════════════════════════════

TEST(MetricsReport, MaxDrawdown_PeakAndTrough) {
    MetricsReport r;
    r.record_pnl(100);   // cumulative=100, peak=100, dd=0
    r.record_pnl(-50);   // cumulative=50,  peak=100, dd=50/100=0.5
    r.record_pnl(200);   // cumulative=250, peak=250, dd=0
    r.record_pnl(-300);  // cumulative=-50, peak=250, dd=300/250=1.2

    EXPECT_NEAR(r.max_drawdown(), 1.2, 1e-9);
}

TEST(MetricsReport, MaxDrawdown_NoDrawdown) {
    MetricsReport r;
    r.record_pnl(100);
    r.record_pnl(200);
    r.record_pnl(300);
    EXPECT_DOUBLE_EQ(r.max_drawdown(), 0.0);
}

TEST(MetricsReport, MaxDrawdown_AlwaysNegative) {
    // 累计始终 <= 0，peak 不超过 0，max_drawdown 应为 0
    MetricsReport r;
    r.record_pnl(-100);
    r.record_pnl(-200);
    EXPECT_DOUBLE_EQ(r.max_drawdown(), 0.0);
}

TEST(MetricsReport, MaxDrawdown_Empty) {
    MetricsReport r;
    EXPECT_DOUBLE_EQ(r.max_drawdown(), 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. MetricsReport：win_rate 正确
//    [100, -50, 200, 0, -10]: 盈利 2 天（100, 200），总 5 天 → 2/5 = 0.4
// ═══════════════════════════════════════════════════════════════════════════

TEST(MetricsReport, WinRate_Mixed) {
    MetricsReport r;
    r.record_pnl(100);
    r.record_pnl(-50);
    r.record_pnl(200);
    r.record_pnl(0);
    r.record_pnl(-10);

    // win: 100 > 0 ✓, -50 ✗, 200 > 0 ✓, 0 ✗, -10 ✗ → 2/5
    EXPECT_NEAR(r.win_rate(), 0.4, 1e-9);
}

TEST(MetricsReport, WinRate_AllPositive) {
    MetricsReport r;
    r.record_pnl(1);
    r.record_pnl(2);
    r.record_pnl(3);
    EXPECT_DOUBLE_EQ(r.win_rate(), 1.0);
}

TEST(MetricsReport, WinRate_AllNegative) {
    MetricsReport r;
    r.record_pnl(-1);
    r.record_pnl(-2);
    EXPECT_DOUBLE_EQ(r.win_rate(), 0.0);
}

TEST(MetricsReport, WinRate_Empty) {
    MetricsReport r;
    EXPECT_DOUBLE_EQ(r.win_rate(), 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 附加：总 P&L 和边界检查
// ═══════════════════════════════════════════════════════════════════════════

TEST(MetricsReport, TotalPnl) {
    MetricsReport r;
    r.record_pnl(100);
    r.record_pnl(-30);
    r.record_pnl(50);
    EXPECT_EQ(r.total_pnl(),     120);
    EXPECT_EQ(r.max_daily_pnl(), 100);
    EXPECT_EQ(r.min_daily_pnl(), -30);
    EXPECT_EQ(r.trading_days(),  3u);
}

TEST(MetricsReport, EmptyReport) {
    MetricsReport r;
    EXPECT_EQ(r.total_pnl(),    0);
    EXPECT_EQ(r.trading_days(), 0u);
    EXPECT_DOUBLE_EQ(r.sharpe_ratio(), 0.0);
    EXPECT_DOUBLE_EQ(r.win_rate(),     0.0);
    EXPECT_DOUBLE_EQ(r.max_drawdown(), 0.0);
}

} // namespace
} // namespace hft
