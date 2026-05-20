// tests/unit/test_position.cpp
// 持仓管理 & 盈亏计算层单元测试
//
// 覆盖场景：
//   1. 买入开仓：net_qty / avg_cost 正确
//   2. 同向加仓：avg_cost 按加权均价更新
//   3. 部分平仓：realized_pnl 正确，net_qty 减少，avg_cost 不变
//   4. 全部平仓：net_qty = 0，realized_pnl 正确
//   5. 反向翻仓（多 → 空）：net_qty 变负，avg_cost 更新为新仓成本
//   6. 卖出开空 + 空头加仓：avg_cost 加权均价
//   7. 部分平空 / 全部平空 / 空头翻多
//   8. unrealized_pnl（多头 / 空头）
//   9. total_pnl = realized + unrealized
//  10. 多品种互不干扰
//  11. 并发两线程各自更新不同品种，无崩溃（false sharing 压力测试）

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

#include "position/position_manager.hpp"
#include "position/pnl_calculator.hpp"
#include "oms/order.hpp"
#include "common/types.hpp"

namespace hft {
namespace {

// ── 辅助函数：构造 FillEvent ──────────────────────────────────────────────

FillEvent make_fill(InstrumentId id, Side side, Price price, Qty qty) noexcept {
    FillEvent f{};
    f.instrument_id = id;
    f.side          = side;
    f.fill_price    = price;
    f.fill_qty      = qty;
    return f;
}

// ── 静态断言：Slot 恰好占一个 cacheline ───────────────────────────────────

static_assert(sizeof(PositionManager::Slot) == CACHELINE_SIZE,
              "Slot must be exactly CACHELINE_SIZE bytes");

// ═════════════════════════════════════════════════════════════════════════════
// 1. 买入开仓
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, BuyOpenPosition) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY, /*price=*/100, /*qty=*/100));

    EXPECT_EQ(pm.get_net_qty(0),       100);
    EXPECT_EQ(pm.get_avg_cost(0),      100);
    EXPECT_EQ(pm.get_realized_pnl(0),    0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. 同向加仓：avg_cost 加权平均
//    Buy 100 @ 100，再 Buy 100 @ 110
//    new_avg = (100*100 + 110*100) / 200 = 21000 / 200 = 105
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, AddToLongPosition_WeightedAvgCost) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY, 100, 100));
    pm.on_fill(make_fill(0, Side::BUY, 110, 100));

    EXPECT_EQ(pm.get_net_qty(0),  200);
    EXPECT_EQ(pm.get_avg_cost(0), 105);   // (10000 + 11000) / 200
    EXPECT_EQ(pm.get_realized_pnl(0), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. 部分平仓：realized_pnl 正确，net_qty 减少，avg_cost 不变
//    Buy 200 @ 100，Sell 50 @ 120
//    realized = (120-100)*50 = 1000，剩余 150 @ 100
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, PartialClose_Long) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY,  100, 200));
    pm.on_fill(make_fill(0, Side::SELL, 120,  50));

    EXPECT_EQ(pm.get_net_qty(0),        150);
    EXPECT_EQ(pm.get_avg_cost(0),       100);   // avg_cost 不变
    EXPECT_EQ(pm.get_realized_pnl(0), 1000);    // (120-100)*50
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. 全部平仓：net_qty = 0，realized_pnl 正确，avg_cost 清零
//    Buy 100 @ 100，Sell 100 @ 130
//    realized = (130-100)*100 = 3000
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, FullClose_Long) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(0, Side::SELL, 130, 100));

    EXPECT_EQ(pm.get_net_qty(0),         0);
    EXPECT_EQ(pm.get_avg_cost(0),        0);
    EXPECT_EQ(pm.get_realized_pnl(0), 3000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. 多头翻空（超额平多 → 开空）
//    Buy 100 @ 100，Sell 150 @ 120
//    平多 100：realized = (120-100)*100 = 2000
//    剩余 50 开空 @ 120：net_qty = -50，avg_cost = 120
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, LongToShortReversal) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(0, Side::SELL, 120, 150));

    EXPECT_EQ(pm.get_net_qty(0),        -50);
    EXPECT_EQ(pm.get_avg_cost(0),        120);
    EXPECT_EQ(pm.get_realized_pnl(0), 2000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. 卖出开空 + 空头加仓
//    Sell 100 @ 120，再 Sell 100 @ 110
//    new_avg = (120*100 + 110*100) / 200 = 23000 / 200 = 115
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, SellOpenShort_ThenAddToShort) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::SELL, 120, 100));
    pm.on_fill(make_fill(0, Side::SELL, 110, 100));

    EXPECT_EQ(pm.get_net_qty(0),  -200);
    EXPECT_EQ(pm.get_avg_cost(0),  115);
    EXPECT_EQ(pm.get_realized_pnl(0), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7a. 部分平空
//     Sell 100 @ 120，Buy 50 @ 100
//     realized = (120-100)*50 = 1000，剩余空头 -50 @ 120
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, PartialClose_Short) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::SELL, 120, 100));
    pm.on_fill(make_fill(0, Side::BUY,  100,  50));

    EXPECT_EQ(pm.get_net_qty(0),         -50);
    EXPECT_EQ(pm.get_avg_cost(0),        120);   // avg_cost 不变
    EXPECT_EQ(pm.get_realized_pnl(0), 1000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7b. 全部平空
//     Sell 100 @ 120，Buy 100 @ 100
//     realized = (120-100)*100 = 2000，net_qty = 0
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, FullClose_Short) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::SELL, 120, 100));
    pm.on_fill(make_fill(0, Side::BUY,  100, 100));

    EXPECT_EQ(pm.get_net_qty(0),         0);
    EXPECT_EQ(pm.get_avg_cost(0),        0);
    EXPECT_EQ(pm.get_realized_pnl(0), 2000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7c. 空头翻多（超额平空 → 开多）
//     Sell 100 @ 120，Buy 150 @ 100
//     平空 100：realized = (120-100)*100 = 2000
//     剩余 50 开多 @ 100：net_qty = 50，avg_cost = 100
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, ShortToLongReversal) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::SELL, 120, 100));
    pm.on_fill(make_fill(0, Side::BUY,  100, 150));

    EXPECT_EQ(pm.get_net_qty(0),       50);
    EXPECT_EQ(pm.get_avg_cost(0),     100);
    EXPECT_EQ(pm.get_realized_pnl(0), 2000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 8. unrealized_pnl
//    多头：Buy 100 @ 100，mark = 130 → unrealized = (130-100)*100 = 3000
//    空头：Sell 100 @ 120，mark = 100 → unrealized = (100-120)*(-100) = 2000
// ═════════════════════════════════════════════════════════════════════════════

TEST(PnlCalculator, UnrealizedPnl_Long) {
    PositionManager pm;
    PnlCalculator   calc(pm);

    pm.on_fill(make_fill(0, Side::BUY, 100, 100));

    EXPECT_EQ(calc.calc_unrealized_pnl(0, /*mark=*/130), 3000);
    EXPECT_EQ(calc.calc_unrealized_pnl(0, /*mark=*/100),    0);
    EXPECT_EQ(calc.calc_unrealized_pnl(0, /*mark=*/80),  -2000);
}

TEST(PnlCalculator, UnrealizedPnl_Short) {
    PositionManager pm;
    PnlCalculator   calc(pm);

    pm.on_fill(make_fill(0, Side::SELL, 120, 100));

    // mark = 100: (100-120)*(-100) = 2000  → 空头盈利
    EXPECT_EQ(calc.calc_unrealized_pnl(0, /*mark=*/100),  2000);
    // mark = 120: no PnL
    EXPECT_EQ(calc.calc_unrealized_pnl(0, /*mark=*/120),     0);
    // mark = 140: (140-120)*(-100) = -2000 → 空头亏损
    EXPECT_EQ(calc.calc_unrealized_pnl(0, /*mark=*/140), -2000);
}

TEST(PnlCalculator, UnrealizedPnl_Flat) {
    PositionManager pm;
    PnlCalculator   calc(pm);
    // 无持仓
    EXPECT_EQ(calc.calc_unrealized_pnl(0, 200), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 9. total_pnl = realized + unrealized
//    Buy 100 @ 100，Sell 50 @ 120（realized=1000，剩余 50 多头 @ 100）
//    mark_prices[0] = 130：unrealized = (130-100)*50 = 1500
//    total = 1000 + 1500 = 2500
// ═════════════════════════════════════════════════════════════════════════════

TEST(PnlCalculator, TotalPnl) {
    PositionManager pm;
    PnlCalculator   calc(pm);

    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(0, Side::SELL, 120,  50));

    Price mark_prices[1] = {130};
    EXPECT_EQ(calc.calc_total_pnl(mark_prices, 1), 2500);
}

// ═════════════════════════════════════════════════════════════════════════════
// 10. 多品种互不干扰
//     instrument 0：Buy 100 @ 100
//     instrument 1：Sell 200 @ 50
//     验证两者状态独立
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, MultipleInstruments_Independent) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(1, Side::SELL,  50, 200));

    // instrument 0
    EXPECT_EQ(pm.get_net_qty(0),       100);
    EXPECT_EQ(pm.get_avg_cost(0),      100);
    EXPECT_EQ(pm.get_realized_pnl(0),    0);

    // instrument 1
    EXPECT_EQ(pm.get_net_qty(1),      -200);
    EXPECT_EQ(pm.get_avg_cost(1),       50);
    EXPECT_EQ(pm.get_realized_pnl(1),    0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 10b. total_realized_pnl 跨品种汇总
//      instrument 0：Buy 100 @ 100，Sell 100 @ 110 → realized = 1000
//      instrument 2：Sell 100 @ 200，Buy 100 @ 150  → realized = 5000
//      total = 6000
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, TotalRealizedPnl_MultiInstrument) {
    PositionManager pm;

    // instrument 0：多头平仓盈利 1000
    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(0, Side::SELL, 110, 100));

    // instrument 2：空头平仓盈利 5000
    pm.on_fill(make_fill(2, Side::SELL, 200, 100));
    pm.on_fill(make_fill(2, Side::BUY,  150, 100));

    EXPECT_EQ(pm.get_realized_pnl(0), 1000);
    EXPECT_EQ(pm.get_realized_pnl(2), 5000);
    EXPECT_EQ(pm.get_total_realized_pnl(), 6000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 10c. calc_total_unrealized_pnl 跨品种汇总
//      instrument 0：Buy 100 @ 100，mark=110 → +1000
//      instrument 1：Sell 50 @ 200，mark=190 → (190-200)*(-50) = 500
//      total_unrealized = 1500
// ═════════════════════════════════════════════════════════════════════════════

TEST(PnlCalculator, TotalUnrealizedPnl_MultiInstrument) {
    PositionManager pm;
    PnlCalculator   calc(pm);

    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(1, Side::SELL, 200,  50));

    Price marks[2] = {110, 190};
    EXPECT_EQ(calc.calc_total_unrealized_pnl(marks, 2), 1500);
}

// ═════════════════════════════════════════════════════════════════════════════
// 11. 并发 false sharing 压力测试
//     两个线程各自更新不同品种（0 和 1），验证无崩溃 / 数据损坏。
//     由于 on_fill 设计为单写者，此处使用 instrument 0 / 1 由各线程独享，
//     验证 cacheline 隔离不引起伪竞争导致的 UB。
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, ConcurrentDifferentInstruments_NoDataRace) {
    PositionManager pm;

    constexpr int kIter = 100'000;
    std::atomic<bool> start_flag{false};

    auto worker = [&](InstrumentId id) {
        // 等待两个线程同时出发，增加竞态压力
        while (!start_flag.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kIter; ++i) {
            // 交替买卖，保持净仓接近 0，避免溢出
            pm.on_fill(make_fill(id, Side::BUY,  100, 1));
            pm.on_fill(make_fill(id, Side::SELL, 100, 1));
        }
    };

    std::thread t0(worker, 0);
    std::thread t1(worker, 1);

    start_flag.store(true, std::memory_order_release);

    t0.join();
    t1.join();

    // 两个品种净仓均应归零（买卖对称）
    EXPECT_EQ(pm.get_net_qty(0), 0);
    EXPECT_EQ(pm.get_net_qty(1), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 12. 边界：instrument_id 越界不崩溃
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, OutOfBounds_InstrumentId) {
    PositionManager pm;
    // 越界 id：不应崩溃，读接口返回 0
    pm.on_fill(make_fill(MAX_INSTRUMENTS, Side::BUY, 100, 100));
    EXPECT_EQ(pm.get_net_qty(MAX_INSTRUMENTS),        0);
    EXPECT_EQ(pm.get_avg_cost(MAX_INSTRUMENTS),       0);
    EXPECT_EQ(pm.get_realized_pnl(MAX_INSTRUMENTS),   0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 13. 连续多次买卖的累计 realized_pnl 正确性
//     三次交易：Buy 100@100, Sell 50@110, Sell 50@120
//     realized = (110-100)*50 + (120-100)*50 = 500 + 1000 = 1500
// ═════════════════════════════════════════════════════════════════════════════

TEST(PositionManager, AccumulatedRealizedPnl) {
    PositionManager pm;
    pm.on_fill(make_fill(0, Side::BUY,  100, 100));
    pm.on_fill(make_fill(0, Side::SELL, 110,  50));
    pm.on_fill(make_fill(0, Side::SELL, 120,  50));

    EXPECT_EQ(pm.get_net_qty(0),          0);
    EXPECT_EQ(pm.get_avg_cost(0),         0);
    EXPECT_EQ(pm.get_realized_pnl(0), 1500);
}

} // namespace
} // namespace hft
