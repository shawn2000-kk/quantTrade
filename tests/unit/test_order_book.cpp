// tests/unit/test_order_book.cpp
// GoogleTest 单元测试：OrderBook + BookBuilder
//
// 覆盖：
//   1. apply_bbo  → mid_price / spread 正确
//   2. apply_depth → snapshot_depth 与输入一致
//   3. is_crossed  → 穿越行情检测
//   4. BookBuilder → 多品种路由隔离
//   5. 并发读写    → 写线程持续 apply_bbo，读线程持续读 mid_price，跑 10 万次无崩溃

#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <thread>

#include "market_data/order_book.hpp"
#include "market_data/book_builder.hpp"

namespace hft {

// ══════════════════════════════════════════════════════════════════
// OrderBook 基本正确性
// ══════════════════════════════════════════════════════════════════

// ── 1a. apply_bbo → mid_price ─────────────────────────────────────
TEST(OrderBookTest, ApplyBboMidPrice) {
    OrderBook book(42);

    BBOEvent ev{};
    ev.instrument_id = 42;
    ev.type          = EventType::BBO_UPDATE;
    ev.bid_px        = 10000;
    ev.ask_px        = 10002;
    ev.bid_qty       = 100;
    ev.ask_qty       = 200;

    book.apply_bbo(ev);

    // mid = (10000 + 10002) / 2 = 10001
    EXPECT_EQ(book.mid_price(), Price{10001});
}

// ── 1b. apply_bbo → spread ────────────────────────────────────────
TEST(OrderBookTest, ApplyBboSpread) {
    OrderBook book(1);

    BBOEvent ev{};
    ev.instrument_id = 1;
    ev.type          = EventType::BBO_UPDATE;
    ev.bid_px        = 9998;
    ev.ask_px        = 10003;
    ev.bid_qty       = 50;
    ev.ask_qty       = 80;

    book.apply_bbo(ev);

    // spread = ask - bid = 10003 - 9998 = 5
    EXPECT_EQ(book.spread(), Price{5});
}

// ── 1c. apply_bbo → snapshot_bbo 与输入一致 ────────────────────────
TEST(OrderBookTest, ApplyBboSnapshotConsistency) {
    OrderBook book(7);

    BBOEvent in_ev{};
    in_ev.instrument_id = 7;
    in_ev.type          = EventType::BBO_UPDATE;
    in_ev.bid_px        = 50000;
    in_ev.ask_px        = 50010;
    in_ev.bid_qty       = 300;
    in_ev.ask_qty       = 150;

    book.apply_bbo(in_ev);
    BBOEvent out_ev = book.snapshot_bbo();

    EXPECT_EQ(out_ev.instrument_id, InstrumentId{7});
    EXPECT_EQ(out_ev.bid_px,  Price{50000});
    EXPECT_EQ(out_ev.ask_px,  Price{50010});
    EXPECT_EQ(out_ev.bid_qty, Qty{300});
    EXPECT_EQ(out_ev.ask_qty, Qty{150});
    EXPECT_EQ(out_ev.type, EventType::BBO_UPDATE);
}

// ── 1d. 未设置时 mid_price / spread 返回 0 ───────────────────────
TEST(OrderBookTest, EmptyBookReturnsZero) {
    OrderBook book(99);
    EXPECT_EQ(book.mid_price(), Price{0});
    EXPECT_EQ(book.spread(),    Price{0});
    EXPECT_FALSE(book.is_crossed());
}

// ── 1e. 多次 apply_bbo 覆盖旧值 ──────────────────────────────────
TEST(OrderBookTest, ApplyBboOverwrite) {
    OrderBook book(2);

    BBOEvent ev1{};
    ev1.instrument_id = 2;
    ev1.bid_px        = 100;
    ev1.ask_px        = 101;
    ev1.bid_qty       = 10;
    ev1.ask_qty       = 20;
    book.apply_bbo(ev1);

    BBOEvent ev2{};
    ev2.instrument_id = 2;
    ev2.bid_px        = 200;
    ev2.ask_px        = 202;
    ev2.bid_qty       = 30;
    ev2.ask_qty       = 40;
    book.apply_bbo(ev2);

    // 第二次 BBO 应覆盖第一次
    EXPECT_EQ(book.mid_price(), Price{201});
    EXPECT_EQ(book.spread(),    Price{2});
}

// ══════════════════════════════════════════════════════════════════
// apply_depth → snapshot_depth 一致性
// ══════════════════════════════════════════════════════════════════

// ── 2a. 完整 5 档 ─────────────────────────────────────────────────
TEST(OrderBookTest, ApplyDepthSnapshotFull5Levels) {
    OrderBook book(10);

    DepthEvent in_d{};
    in_d.instrument_id = 10;
    in_d.bid_levels    = 5;
    in_d.ask_levels    = 5;
    for (uint8_t i = 0; i < 5; ++i) {
        in_d.bids[i] = {static_cast<Price>(1000 - i * 2), static_cast<Qty>(100 + i * 10)};
        in_d.asks[i] = {static_cast<Price>(1001 + i * 2), static_cast<Qty>(200 + i * 10)};
    }

    book.apply_depth(in_d);
    DepthEvent out_d = book.snapshot_depth();

    EXPECT_EQ(out_d.instrument_id, InstrumentId{10});
    EXPECT_EQ(out_d.bid_levels, uint8_t{5});
    EXPECT_EQ(out_d.ask_levels, uint8_t{5});

    for (uint8_t i = 0; i < 5; ++i) {
        EXPECT_EQ(out_d.bids[i].price, in_d.bids[i].price)
            << "bid[" << +i << "] price mismatch";
        EXPECT_EQ(out_d.bids[i].qty,   in_d.bids[i].qty)
            << "bid[" << +i << "] qty mismatch";
        EXPECT_EQ(out_d.asks[i].price, in_d.asks[i].price)
            << "ask[" << +i << "] price mismatch";
        EXPECT_EQ(out_d.asks[i].qty,   in_d.asks[i].qty)
            << "ask[" << +i << "] qty mismatch";
    }
}

// ── 2b. 部分档位（bid_levels=3，ask_levels=2）────────────────────
TEST(OrderBookTest, ApplyDepthPartialLevels) {
    OrderBook book(11);

    DepthEvent in_d{};
    in_d.instrument_id = 11;
    in_d.bid_levels    = 3;
    in_d.ask_levels    = 2;
    in_d.bids[0] = {5000, 100};
    in_d.bids[1] = {4998, 200};
    in_d.bids[2] = {4996, 300};
    in_d.asks[0] = {5002, 150};
    in_d.asks[1] = {5004, 250};

    book.apply_depth(in_d);
    DepthEvent out_d = book.snapshot_depth();

    EXPECT_EQ(out_d.bid_levels, uint8_t{3});
    EXPECT_EQ(out_d.ask_levels, uint8_t{2});
    EXPECT_EQ(out_d.bids[0].price, Price{5000});
    EXPECT_EQ(out_d.bids[2].price, Price{4996});
    EXPECT_EQ(out_d.asks[0].price, Price{5002});
    EXPECT_EQ(out_d.asks[1].qty,   Qty{250});
}

// ── 2c. apply_depth 后 mid_price / spread 也从第 0 档计算 ──────────
TEST(OrderBookTest, ApplyDepthMidSpread) {
    OrderBook book(12);

    DepthEvent d{};
    d.instrument_id = 12;
    d.bid_levels    = 5;
    d.ask_levels    = 5;
    d.bids[0] = {2000, 10};
    d.asks[0] = {2004, 20};
    // 其余档位留 0

    book.apply_depth(d);

    EXPECT_EQ(book.mid_price(), Price{2002});
    EXPECT_EQ(book.spread(),    Price{4});
}

// ══════════════════════════════════════════════════════════════════
// is_crossed：穿越行情检测
// ══════════════════════════════════════════════════════════════════

// ── 3a. 正常行情（bid < ask）→ 不 crossed ─────────────────────────
TEST(OrderBookTest, IsCrossedNormal) {
    OrderBook book(20);
    BBOEvent ev{};
    ev.instrument_id = 20;
    ev.bid_px        = 9999;
    ev.ask_px        = 10001;
    ev.bid_qty       = 1;
    ev.ask_qty       = 1;
    book.apply_bbo(ev);
    EXPECT_FALSE(book.is_crossed());
}

// ── 3b. bid > ask → crossed ───────────────────────────────────────
TEST(OrderBookTest, IsCrossedWhenBidGreaterThanAsk) {
    OrderBook book(21);
    BBOEvent ev{};
    ev.instrument_id = 21;
    ev.bid_px        = 10010;   // bid 高于 ask → 穿越
    ev.ask_px        = 10005;
    ev.bid_qty       = 1;
    ev.ask_qty       = 1;
    book.apply_bbo(ev);
    EXPECT_TRUE(book.is_crossed());
}

// ── 3c. bid == ask → crossed（等价穿越） ──────────────────────────
TEST(OrderBookTest, IsCrossedWhenBidEqualsAsk) {
    OrderBook book(22);
    BBOEvent ev{};
    ev.instrument_id = 22;
    ev.bid_px        = 10000;
    ev.ask_px        = 10000;
    ev.bid_qty       = 1;
    ev.ask_qty       = 1;
    book.apply_bbo(ev);
    EXPECT_TRUE(book.is_crossed());
}

// ── 3d. spread 为负（交叉行情）时 spread() 返回负值 ─────────────────
TEST(OrderBookTest, SpreadNegativeWhenCrossed) {
    OrderBook book(23);
    BBOEvent ev{};
    ev.instrument_id = 23;
    ev.bid_px        = 500;
    ev.ask_px        = 498;   // ask < bid
    ev.bid_qty       = 1;
    ev.ask_qty       = 1;
    book.apply_bbo(ev);
    EXPECT_LT(book.spread(), Price{0});
    EXPECT_TRUE(book.is_crossed());
}

// ══════════════════════════════════════════════════════════════════
// BookBuilder：多品种路由
// ══════════════════════════════════════════════════════════════════

// ── 4a. 不同品种的 OrderBook 相互独立 ─────────────────────────────
TEST(BookBuilderTest, MultiInstrumentIsolation) {
    BookBuilder builder;

    BBOEvent ev_a{};
    ev_a.instrument_id = 100;
    ev_a.bid_px        = 1000;
    ev_a.ask_px        = 1002;
    ev_a.bid_qty       = 50;
    ev_a.ask_qty       = 60;

    BBOEvent ev_b{};
    ev_b.instrument_id = 200;
    ev_b.bid_px        = 5000;
    ev_b.ask_px        = 5005;
    ev_b.bid_qty       = 10;
    ev_b.ask_qty       = 20;

    builder.on_bbo(ev_a);
    builder.on_bbo(ev_b);

    const OrderBook* book_a = builder.find(100);
    const OrderBook* book_b = builder.find(200);

    ASSERT_NE(book_a, nullptr);
    ASSERT_NE(book_b, nullptr);
    EXPECT_NE(book_a, book_b);

    // 品种 A 的数据
    EXPECT_EQ(book_a->mid_price(), Price{1001});
    EXPECT_EQ(book_a->spread(),    Price{2});

    // 品种 B 的数据（不受 A 影响）
    EXPECT_EQ(book_b->mid_price(), Price{5002});  // (5000+5005)/2 = 5002
    EXPECT_EQ(book_b->spread(),    Price{5});
}

// ── 4b. find 找不到的品种返回 nullptr ─────────────────────────────
TEST(BookBuilderTest, FindReturnsNullForUnknownInstrument) {
    BookBuilder builder;
    EXPECT_EQ(builder.find(InstrumentId{9999}), nullptr);
}

// ── 4c. get_or_create 懒初始化，同品种返回同一引用 ─────────────────
TEST(BookBuilderTest, GetOrCreateReturnsSameRef) {
    BookBuilder builder;
    OrderBook& ref1 = builder.get_or_create(42);
    OrderBook& ref2 = builder.get_or_create(42);
    EXPECT_EQ(&ref1, &ref2);
    EXPECT_EQ(builder.book_count(), size_t{1});
}

// ── 4d. DepthEvent 路由到正确品种 ─────────────────────────────────
TEST(BookBuilderTest, DepthRoutedCorrectly) {
    BookBuilder builder;

    DepthEvent d{};
    d.instrument_id = 300;
    d.bid_levels    = 3;
    d.ask_levels    = 3;
    d.bids[0] = {8000, 100};
    d.bids[1] = {7998, 200};
    d.bids[2] = {7996, 300};
    d.asks[0] = {8002, 150};
    d.asks[1] = {8004, 250};
    d.asks[2] = {8006, 350};

    builder.on_depth(d);

    const OrderBook* book = builder.find(300);
    ASSERT_NE(book, nullptr);

    DepthEvent snap = book->snapshot_depth();
    EXPECT_EQ(snap.bid_levels, uint8_t{3});
    EXPECT_EQ(snap.ask_levels, uint8_t{3});
    EXPECT_EQ(snap.bids[0].price, Price{8000});
    EXPECT_EQ(snap.asks[0].price, Price{8002});
}

// ── 4e. 多品种同时存在，book_count 正确 ──────────────────────────
TEST(BookBuilderTest, BookCountCorrect) {
    BookBuilder builder;
    for (InstrumentId id = 1; id <= 10; ++id) {
        BBOEvent ev{};
        ev.instrument_id = id;
        ev.bid_px        = static_cast<Price>(id * 100);
        ev.ask_px        = static_cast<Price>(id * 100 + 1);
        ev.bid_qty       = 1;
        ev.ask_qty       = 1;
        builder.on_bbo(ev);
    }
    EXPECT_EQ(builder.book_count(), size_t{10});
}

// ══════════════════════════════════════════════════════════════════
// 并发读写：1 写线程 apply_bbo + 1 读线程 mid_price，10 万次，无崩溃
// ══════════════════════════════════════════════════════════════════

TEST(OrderBookConcurrencyTest, ConcurrentBboWriteAndMidPriceRead) {
    constexpr int ITERATIONS = 100'000;

    OrderBook book(999);

    // 先写入初始值，保证读线程有有效数据
    BBOEvent init_ev{};
    init_ev.instrument_id = 999;
    init_ev.bid_px        = 10000;
    init_ev.ask_px        = 10002;
    init_ev.bid_qty       = 100;
    init_ev.ask_qty       = 100;
    book.apply_bbo(init_ev);

    std::atomic<bool> start_flag{false};
    std::atomic<int>  read_count{0};

    // 写线程：持续更新 BBO（交替两组价格）
    std::thread writer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {}
        for (int i = 0; i < ITERATIONS; ++i) {
            BBOEvent ev{};
            ev.instrument_id = 999;
            if (i % 2 == 0) {
                ev.bid_px  = 10000;
                ev.ask_px  = 10002;
            } else {
                ev.bid_px  = 20000;
                ev.ask_px  = 20004;
            }
            ev.bid_qty = 50;
            ev.ask_qty = 50;
            book.apply_bbo(ev);
        }
    });

    // 读线程：持续读取 mid_price，累计有效读取次数
    std::thread reader([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {}
        for (int i = 0; i < ITERATIONS; ++i) {
            Price mp = book.mid_price();
            // 只要返回有效的非零 mid_price 即算一次成功读取
            if (mp > 0) {
                read_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // 同时启动两个线程
    start_flag.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    // 验证：没有崩溃；读线程应读到大量有效数据（> 0 次）
    EXPECT_GT(read_count.load(), 0)
        << "Reader got no valid mid_price in " << ITERATIONS << " iterations";
}

// ── 并发读写 snapshot_depth ───────────────────────────────────────
TEST(OrderBookConcurrencyTest, ConcurrentDepthWriteAndSnapshotRead) {
    constexpr int ITERATIONS = 100'000;

    OrderBook book(998);
    std::atomic<bool> start_flag{false};

    // 写线程：持续更新 depth
    std::thread writer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {}
        for (int i = 0; i < ITERATIONS; ++i) {
            DepthEvent d{};
            d.instrument_id = 998;
            d.bid_levels    = 5;
            d.ask_levels    = 5;
            for (uint8_t lv = 0; lv < 5; ++lv) {
                d.bids[lv] = {static_cast<Price>(1000 - lv * 2 + i % 10), Qty{100}};
                d.asks[lv] = {static_cast<Price>(1001 + lv * 2 + i % 10), Qty{100}};
            }
            book.apply_depth(d);
        }
    });

    // 读线程：持续拿快照，只要不崩溃即可
    std::thread reader([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {}
        for (int i = 0; i < ITERATIONS; ++i) {
            DepthEvent snap = book.snapshot_depth();
            // 每次取到的 bid_levels / ask_levels 必须 ≤ MARKET_DEPTH
            ASSERT_LE(snap.bid_levels, static_cast<uint8_t>(MARKET_DEPTH));
            ASSERT_LE(snap.ask_levels, static_cast<uint8_t>(MARKET_DEPTH));
        }
    });

    start_flag.store(true, std::memory_order_release);
    writer.join();
    reader.join();
}

}  // namespace hft
