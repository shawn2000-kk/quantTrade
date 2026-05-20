#include <gtest/gtest.h>
#include <type_traits>

#include "common/types.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"
#include "strategy/strategy_base.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/strategy_manager.hpp"

namespace hft {
namespace {

// ── 辅助：构造一个标准 BBO 事件 ─────────────────────────────────────────
static BBOEvent make_bbo(InstrumentId id, Price bid, Price ask) noexcept {
    BBOEvent e{};
    e.instrument_id = id;
    e.bid_px        = bid;
    e.ask_px        = ask;
    e.type          = EventType::BBO_UPDATE;
    return e;
}

// ── 辅助：构造一个成交回报 ───────────────────────────────────────────────
static FillEvent make_fill(InstrumentId id, Side side, Qty qty, Price px) noexcept {
    FillEvent f{};
    f.instrument_id = id;
    f.side          = side;
    f.fill_qty      = qty;
    f.fill_price    = px;
    return f;
}

// ════════════════════════════════════════════════════════════════════════
// 套件一：CRTP / 无虚函数验证
// ════════════════════════════════════════════════════════════════════════

TEST(CRTPTest, MarketMakerHasNoVtable) {
    // MarketMaker 使用 CRTP 静态分派，不含虚表
    static_assert(!std::is_polymorphic_v<MarketMaker>,
        "MarketMaker must not be polymorphic (no virtual functions)");
    EXPECT_FALSE(std::is_polymorphic_v<MarketMaker>);
}

TEST(CRTPTest, SizeDoesNotContainVptr) {
    // 有 vtable 时 sizeof 会多出一个指针（8 bytes on 64-bit）
    // 此测试确认尺寸仅由成员决定，不含 vptr
    MarketMaker mm(1, 2, 10, 100);
    (void)mm;
    EXPECT_FALSE(std::is_polymorphic_v<MarketMaker>);
}

// ════════════════════════════════════════════════════════════════════════
// 套件二：MarketMaker 基础报单行为
// ════════════════════════════════════════════════════════════════════════

// 收到有效 BBO 后，应分别产生一笔 BUY 限价单和一笔 SELL 限价单
TEST(MarketMakerTest, PlacesBidAndAskOnFirstBBO) {
    MarketMaker mm(1, 2, 10, 100);

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid))  << "Expected bid order in queue";
    ASSERT_TRUE(mm.pop_order(ask))  << "Expected ask order in queue";

    // 第三次 pop 应为空
    OrderRequest extra{};
    EXPECT_FALSE(mm.pop_order(extra)) << "No more orders expected";

    EXPECT_EQ(bid.side, Side::BUY);
    EXPECT_EQ(ask.side, Side::SELL);

    EXPECT_EQ(bid.type, OrderType::LIMIT);
    EXPECT_EQ(ask.type, OrderType::LIMIT);

    EXPECT_EQ(bid.instrument_id, InstrumentId{1});
    EXPECT_EQ(ask.instrument_id, InstrumentId{1});

    EXPECT_EQ(bid.qty, Qty{10});
    EXPECT_EQ(ask.qty, Qty{10});
}

// 价格计算验证：bid = mid - spread/2，ask = mid + spread/2
// bid=100, ask=102 → mid=101, spread=2, half=1 → bid_price=100, ask_price=102
TEST(MarketMakerTest, PriceCalculationSymmetric) {
    MarketMaker mm(1, 2, 10, 100);
    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    EXPECT_EQ(bid.price, Price{100});
    EXPECT_EQ(ask.price, Price{102});
}

// 奇数 spread（3 ticks）：half=1 分给 bid，剩余 2 分给 ask
// bid=100, ask=106 → mid=103, spread=3, half=1 → bid=102, ask=105
TEST(MarketMakerTest, PriceCalculationOddSpread) {
    MarketMaker mm(1, 3, 5, 100);
    mm.on_bbo(make_bbo(1, 100, 106));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    // mid = (100+106)/2 = 103, half_spread = 3/2 = 1
    // bid = 103 - 1 = 102, ask = 103 + (3-1) = 105
    EXPECT_EQ(bid.price, Price{102});
    EXPECT_EQ(ask.price, Price{105});
}

// 同一 mid 不重新报价（delta == 0 时跳过）
TEST(MarketMakerTest, NoRequoteWhenMidUnchanged) {
    MarketMaker mm(1, 2, 10, 100);

    mm.on_bbo(make_bbo(1, 100, 102));

    // 清空第一次的报单
    OrderRequest req{};
    while (mm.pop_order(req)) {}

    // 发送相同 BBO
    mm.on_bbo(make_bbo(1, 100, 102));

    // 不应产生新报单
    EXPECT_FALSE(mm.pop_order(req)) << "Should not requote when mid unchanged";
}

// mid 变化 0.5 tick（sum 变化 1）应触发重新报价
TEST(MarketMakerTest, RequoteOnHalfTickMove) {
    MarketMaker mm(1, 2, 10, 100);

    // bid=100, ask=102 → sum=202
    mm.on_bbo(make_bbo(1, 100, 102));
    OrderRequest req{};
    while (mm.pop_order(req)) {}  // 清空

    // bid=101, ask=102 → sum=203（mid 移动 0.5 tick）
    mm.on_bbo(make_bbo(1, 101, 102));
    EXPECT_TRUE(mm.pop_order(req)) << "Should requote on 0.5 tick mid move";
}

// ════════════════════════════════════════════════════════════════════════
// 套件三：库存超限暂停报单
// ════════════════════════════════════════════════════════════════════════

// 多头超限（inventory >= max_inventory）时，只报 ask，不报 bid
TEST(MarketMakerTest, LongInventoryLimitStopsBid) {
    constexpr Qty max_inv = 5;
    MarketMaker mm(1, 2, 10, max_inv);

    // 通过 on_fill 把 inventory 打到 max_inv + 1 = 6
    const FillEvent buy_fill = make_fill(1, Side::BUY, 1, 100);
    for (int i = 0; i < static_cast<int>(max_inv) + 1; ++i) {
        mm.on_fill(buy_fill);
    }

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest req{};
    ASSERT_TRUE(mm.pop_order(req)) << "Expected ask order despite long inventory";
    EXPECT_EQ(req.side, Side::SELL) << "Only ask should be placed when long limit hit";

    // 不应有第二笔报单
    EXPECT_FALSE(mm.pop_order(req)) << "No bid expected when inventory >= max_inventory";
}

// 空头超限（inventory <= -max_inventory）时，只报 bid，不报 ask
TEST(MarketMakerTest, ShortInventoryLimitStopsAsk) {
    constexpr Qty max_inv = 5;
    MarketMaker mm(1, 2, 10, max_inv);

    // 通过 on_fill 把 inventory 打到 -(max_inv + 1) = -6
    const FillEvent sell_fill = make_fill(1, Side::SELL, 1, 100);
    for (int i = 0; i < static_cast<int>(max_inv) + 1; ++i) {
        mm.on_fill(sell_fill);
    }

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest req{};
    ASSERT_TRUE(mm.pop_order(req)) << "Expected bid order despite short inventory";
    EXPECT_EQ(req.side, Side::BUY) << "Only bid should be placed when short limit hit";

    EXPECT_FALSE(mm.pop_order(req)) << "No ask expected when inventory <= -max_inventory";
}

// 恰好等于 max_inventory 时仍不再报 bid（边界检查：inv < max_inv，非 <=）
TEST(MarketMakerTest, ExactlyAtMaxInventoryStopsBid) {
    constexpr Qty max_inv = 3;
    MarketMaker mm(1, 2, 10, max_inv);

    const FillEvent buy_fill = make_fill(1, Side::BUY, 1, 100);
    for (int i = 0; i < static_cast<int>(max_inv); ++i) {
        mm.on_fill(buy_fill);
    }
    // inventory == max_inv == 3

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest req{};
    // inv == max_inv：bid 条件 (inv < max_inv) 为 false → 只有 ask
    ASSERT_TRUE(mm.pop_order(req));
    EXPECT_EQ(req.side, Side::SELL);
    EXPECT_FALSE(mm.pop_order(req));
}

// ════════════════════════════════════════════════════════════════════════
// 套件四：库存偏斜价格调整
// ════════════════════════════════════════════════════════════════════════

// 多头时 ask 价格下降 1 tick
TEST(MarketMakerTest, InventorySkewLowersAskWhenLong) {
    MarketMaker mm(1, 2, 10, 100);

    // 先建立多头 inventory = 3
    const FillEvent buy_fill = make_fill(1, Side::BUY, 1, 100);
    for (int i = 0; i < 3; ++i) mm.on_fill(buy_fill);

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    // 正常 ask = 102，库存偏斜后 ask = 101
    EXPECT_EQ(bid.price, Price{100});  // bid 不受多头偏斜影响
    EXPECT_EQ(ask.price, Price{101});  // ask 降 1 tick
}

// 空头时 bid 价格上升 1 tick
TEST(MarketMakerTest, InventorySkewRaisesBidWhenShort) {
    MarketMaker mm(1, 2, 10, 100);

    // 先建立空头 inventory = -3
    const FillEvent sell_fill = make_fill(1, Side::SELL, 1, 100);
    for (int i = 0; i < 3; ++i) mm.on_fill(sell_fill);

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    // 正常 bid = 100，库存偏斜后 bid = 101
    EXPECT_EQ(bid.price, Price{101});  // bid 升 1 tick
    EXPECT_EQ(ask.price, Price{102});  // ask 不受空头偏斜影响
}

// ════════════════════════════════════════════════════════════════════════
// 套件五：成交后库存更新
// ════════════════════════════════════════════════════════════════════════

// BUY 成交 → inventory 增加
TEST(MarketMakerTest, FillUpdatesBuyInventory) {
    MarketMaker mm(1, 2, 10, 100);

    // 先报一次单，再成交
    mm.on_bbo(make_bbo(1, 100, 102));
    OrderRequest req{};
    while (mm.pop_order(req)) {}

    mm.on_fill(make_fill(1, Side::BUY, 7, 100));

    // 验证：inventory = 7，下次报价时 ask 应降 1 tick（偏斜生效）
    // 重新触发：先把 mid 改变
    mm.on_bbo(make_bbo(1, 101, 103));  // sum 变化 2，触发重新报价

    OrderRequest bid2{}, ask2{};
    ASSERT_TRUE(mm.pop_order(bid2));
    ASSERT_TRUE(mm.pop_order(ask2));

    // mid = (101+103)/2 = 102, half=1, bid=101, ask=103
    // 多头偏斜: ask -= 1 → ask = 102
    EXPECT_EQ(bid2.price, Price{101});
    EXPECT_EQ(ask2.price, Price{102});
}

// SELL 成交 → inventory 减少
TEST(MarketMakerTest, FillUpdatesSellInventory) {
    MarketMaker mm(1, 2, 10, 100);

    // 建立多头 inventory = 10，然后部分卖出
    for (int i = 0; i < 10; ++i) mm.on_fill(make_fill(1, Side::BUY, 1, 100));
    mm.on_fill(make_fill(1, Side::SELL, 3, 102));
    // inventory = 10 - 3 = 7

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    // inventory = 7 > 0 → ask 偏斜降 1 tick → ask = 101
    EXPECT_EQ(ask.price, Price{101});
}

// 不同品种的成交不影响本策略的 inventory
TEST(MarketMakerTest, FillFromOtherInstrumentIgnored) {
    MarketMaker mm(1, 2, 10, 100);  // instrument 1

    // 发送品种 2 的成交
    mm.on_fill(make_fill(2, Side::BUY, 999, 100));

    mm.on_bbo(make_bbo(1, 100, 102));

    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    // inventory 仍为 0，无偏斜
    EXPECT_EQ(bid.price, Price{100});
    EXPECT_EQ(ask.price, Price{102});
}

// ════════════════════════════════════════════════════════════════════════
// 套件六：OrderAck 更新挂单 ID + 撤单队列
// ════════════════════════════════════════════════════════════════════════

// ACK 后，mid 移动应撤销旧单并报新单；cancel_queue 应含有旧的 order ID
TEST(MarketMakerTest, CancelQueueFilledOnRequote) {
    MarketMaker mm(1, 2, 10, 100);

    // 第一次报价
    mm.on_bbo(make_bbo(1, 100, 102));
    OrderRequest bid{}, ask{};
    ASSERT_TRUE(mm.pop_order(bid));
    ASSERT_TRUE(mm.pop_order(ask));

    // OMS 回 ACK：bid coid=1001, ask coid=1002
    OrderAck bid_ack{};
    bid_ack.client_order_id = 1001;
    bid_ack.status = OrderStatus::NEW;
    mm.on_order_ack(bid_ack);

    OrderAck ask_ack{};
    ask_ack.client_order_id = 1002;
    ask_ack.status = OrderStatus::NEW;
    mm.on_order_ack(ask_ack);

    // mid 移动 1 tick（sum +2），触发重新报价
    mm.on_bbo(make_bbo(1, 101, 103));

    // cancel_queue 应含 1001 和 1002
    uint64_t cid1{}, cid2{};
    ASSERT_TRUE(mm.pop_cancel(cid1)) << "Expected cancel for bid order";
    ASSERT_TRUE(mm.pop_cancel(cid2)) << "Expected cancel for ask order";
    EXPECT_EQ(cid1, uint64_t{1001});
    EXPECT_EQ(cid2, uint64_t{1002});

    // 同时还应产生新报单
    OrderRequest new_bid{};
    EXPECT_TRUE(mm.pop_order(new_bid));
}

// ════════════════════════════════════════════════════════════════════════
// 套件七：非本品种 BBO 被忽略
// ════════════════════════════════════════════════════════════════════════

TEST(MarketMakerTest, BBOForOtherInstrumentIgnored) {
    MarketMaker mm(1, 2, 10, 100);

    // 发送品种 2 的 BBO
    mm.on_bbo(make_bbo(2, 100, 102));

    OrderRequest req{};
    EXPECT_FALSE(mm.pop_order(req)) << "Should ignore BBO for other instrument";
}

// 无效 BBO（bid >= ask）被忽略
TEST(MarketMakerTest, InvalidBBOIgnored) {
    MarketMaker mm(1, 2, 10, 100);

    BBOEvent bad_bbo = make_bbo(1, 102, 100);  // bid > ask，无效
    mm.on_bbo(bad_bbo);

    OrderRequest req{};
    EXPECT_FALSE(mm.pop_order(req)) << "Should ignore invalid BBO";
}

// ════════════════════════════════════════════════════════════════════════
// 套件八：StrategyManager 广播
// ════════════════════════════════════════════════════════════════════════

TEST(StrategyManagerTest, RegisterAndCount) {
    MarketMaker mm1(1, 2, 10, 100);
    MarketMaker mm2(2, 2, 10, 100);

    StrategyManager mgr;
    EXPECT_EQ(mgr.strategy_count(), size_t{0});

    mgr.register_strategy(mm1);
    EXPECT_EQ(mgr.strategy_count(), size_t{1});

    mgr.register_strategy(mm2);
    EXPECT_EQ(mgr.strategy_count(), size_t{2});
}

TEST(StrategyManagerTest, DispatchReachesAllStrategies) {
    MarketMaker mm1(1, 2, 10, 100);  // instrument 1
    MarketMaker mm2(1, 2, 10, 100);  // same instrument

    StrategyManager mgr;
    mgr.register_strategy(mm1);
    mgr.register_strategy(mm2);

    mgr.dispatch_bbo(make_bbo(1, 100, 102));

    // 两个策略都应产生报单
    OrderRequest req{};
    EXPECT_TRUE(mm1.pop_order(req)) << "mm1 should have received BBO";
    EXPECT_TRUE(mm2.pop_order(req)) << "mm2 should have received BBO";
}

TEST(StrategyManagerTest, DispatchFiltersPerStrategy) {
    MarketMaker mm1(1, 2, 10, 100);  // instrument 1
    MarketMaker mm2(2, 2, 10, 100);  // instrument 2

    StrategyManager mgr;
    mgr.register_strategy(mm1);
    mgr.register_strategy(mm2);

    // 只发品种 1 的 BBO
    mgr.dispatch_bbo(make_bbo(1, 100, 102));

    OrderRequest req{};
    EXPECT_TRUE(mm1.pop_order(req))  << "mm1 (instrument 1) should respond";
    EXPECT_FALSE(mm2.pop_order(req)) << "mm2 (instrument 2) should ignore BBO for inst 1";
}

TEST(StrategyManagerTest, DispatchEmptyManagerIsNoop) {
    StrategyManager mgr;
    // 无策略注册，dispatch 不崩溃
    EXPECT_NO_FATAL_FAILURE(mgr.dispatch_bbo(make_bbo(1, 100, 102)));
}

} // anonymous namespace
} // namespace hft
