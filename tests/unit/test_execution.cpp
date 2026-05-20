// tests/unit/test_execution.cpp
// 执行引擎层单元测试：SmartRouter / Twap / Vwap / Pov / ExecutionEngine

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <chrono>
#include <vector>

#include "common/types.hpp"
#include "oms/order.hpp"
#include "connectivity/gateway.hpp"
#include "execution/smart_router.hpp"
#include "execution/twap.hpp"
#include "execution/vwap.hpp"
#include "execution/pov.hpp"
#include "execution/execution_engine.hpp"

namespace hft {
namespace {

// =================================================================
// 测试辅助：MockGateway
//
// 记录每一笔报单的数量，支持手动切换 is_connected 和 get_rtt_ns。
// =================================================================

class MockGateway final : public IGateway {
public:
    explicit MockGateway(bool     connected = true,
                         uint64_t rtt_ns    = 1000,
                         std::string_view nm = "MockGW") noexcept
        : connected_(connected), rtt_ns_(rtt_ns), name_(nm) {}

    void send_new_order(const OrderRequest& req) noexcept override {
        ++order_count_;
        total_qty_ += req.qty;
        last_qty_   = req.qty;
        orders_.push_back(req.qty);
    }

    void send_cancel(uint64_t) noexcept override {}
    void send_modify(const ModifyRequest&) noexcept override {}

    [[nodiscard]] bool        is_connected() const noexcept override { return connected_; }
    [[nodiscard]] uint64_t    get_rtt_ns()   const noexcept override { return rtt_ns_;    }
    [[nodiscard]] std::string_view name()    const noexcept override { return name_;      }

    // ── test helpers ─────────────────────────────────────────────────
    void set_connected(bool v) noexcept { connected_ = v; }
    void set_rtt_ns(uint64_t v) noexcept { rtt_ns_ = v; }

    size_t                    order_count() const noexcept { return order_count_; }
    Qty                       total_qty()   const noexcept { return total_qty_;   }
    Qty                       last_qty()    const noexcept { return last_qty_;    }
    const std::vector<Qty>&   orders()      const noexcept { return orders_;      }
    void                      reset()       noexcept {
        order_count_ = 0; total_qty_ = 0; last_qty_ = 0; orders_.clear();
    }

private:
    bool             connected_;
    uint64_t         rtt_ns_;
    std::string_view name_;
    size_t           order_count_{0};
    Qty              total_qty_{0};
    Qty              last_qty_{0};
    std::vector<Qty> orders_;
};

// 快捷构造 OrderRequest
static OrderRequest make_req(InstrumentId id  = 1,
                              Side         side = Side::BUY,
                              Qty          qty  = 100,
                              Price        px   = 0) noexcept {
    OrderRequest r{};
    r.instrument_id  = id;
    r.side           = side;
    r.type           = OrderType::MARKET;
    r.price          = px;
    r.qty            = qty;
    return r;
}

// =================================================================
// SmartRouter 测试
// =================================================================

TEST(SmartRouterTest, SelectsMinRttConnectedGateway) {
    MockGateway gw_fast(true, 500,  "fast");
    MockGateway gw_slow(true, 2000, "slow");

    SmartRouter router({&gw_slow, &gw_fast});

    // send() 应路由到 RTT 最小的 gw_fast
    router.send(make_req());
    router.send(make_req());
    router.send(make_req());

    EXPECT_EQ(gw_fast.order_count(), 3u);
    EXPECT_EQ(gw_slow.order_count(), 0u);
    EXPECT_EQ(router.dropped_count(), 0u);
}

TEST(SmartRouterTest, SkipsDisconnectedGateway) {
    MockGateway gw_ok  (true,  800,  "ok");
    MockGateway gw_down(false, 100,  "down");   // RTT 更小但断连

    SmartRouter router({&gw_down, &gw_ok});

    router.send(make_req());

    EXPECT_EQ(gw_ok.order_count(),   1u);
    EXPECT_EQ(gw_down.order_count(), 0u);
    EXPECT_EQ(router.dropped_count(), 0u);
}

TEST(SmartRouterTest, AllDisconnectedIncrementsDroppedCount) {
    MockGateway gw1(false, 500);
    MockGateway gw2(false, 800);

    SmartRouter router({&gw1, &gw2});

    router.send(make_req());
    EXPECT_EQ(router.dropped_count(), 1u);

    router.send(make_req());
    router.send(make_req());
    EXPECT_EQ(router.dropped_count(), 3u);

    EXPECT_EQ(gw1.order_count(), 0u);
    EXPECT_EQ(gw2.order_count(), 0u);
}

TEST(SmartRouterTest, SelectReturnsNullptrWhenAllDisconnected) {
    MockGateway gw(false, 300);
    SmartRouter router({&gw});

    IGateway* result = router.select(make_req());
    EXPECT_EQ(result, nullptr);
}

TEST(SmartRouterTest, SelectReturnsCorrectGatewayWhenSingleConnected) {
    MockGateway gw(true, 700, "only");
    SmartRouter router({&gw});

    IGateway* result = router.select(make_req());
    EXPECT_EQ(result, &gw);
}

TEST(SmartRouterTest, DroppedCountInitiallyZero) {
    MockGateway gw(true, 500);
    SmartRouter router({&gw});
    EXPECT_EQ(router.dropped_count(), 0u);
}

TEST(SmartRouterTest, RecoversAfterGatewayReconnects) {
    MockGateway gw(false, 500);
    SmartRouter router({&gw});

    router.send(make_req());
    EXPECT_EQ(router.dropped_count(), 1u);

    gw.set_connected(true);
    router.send(make_req());
    EXPECT_EQ(router.dropped_count(), 1u);
    EXPECT_EQ(gw.order_count(), 1u);
}

// =================================================================
// Twap 测试
// =================================================================

TEST(TwapTest, SendsAllSlicesAndTotalQtyIsExact) {
    MockGateway gw;
    const Qty    total     = 1000;
    const int    slices    = 4;
    // 40ms 总时长，每片 10ms；jitter=0 确保量的确定性
    const uint64_t dur_ns = 40'000'000ULL;

    Twap twap(gw, 1, Side::BUY, total, dur_ns, slices, /*jitter=*/0.0);
    twap.start();

    // spin 直到全部发完（最多等 60ms）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
    while (!twap.is_done() && std::chrono::steady_clock::now() < deadline) {
        twap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(twap.is_done());
    EXPECT_EQ(twap.remaining_qty(), Qty{0});
    EXPECT_EQ(gw.order_count(), static_cast<size_t>(slices));
    EXPECT_EQ(gw.total_qty(), total);
}

TEST(TwapTest, IsNotDoneBeforeStart) {
    MockGateway gw;
    Twap twap(gw, 1, Side::BUY, 100, 10'000'000ULL, 2, 0.0);
    // start() 未调用，is_done() 应为 false（slice_index_ 初始 0，slices=2）
    EXPECT_FALSE(twap.is_done());
}

TEST(TwapTest, FirstTickFiresImmediatelyAfterStart) {
    MockGateway gw;
    Twap twap(gw, 1, Side::BUY, 200, 100'000'000ULL, 4, 0.0);
    twap.start();

    // 第一片时间阈值 = start_ns_，当前时间 >= start_ns_，应立即触发
    const bool fired = twap.tick();
    EXPECT_TRUE(fired);
    EXPECT_EQ(gw.order_count(), 1u);
}

TEST(TwapTest, NoJitterMeansEqualSliceQty) {
    MockGateway gw;
    const Qty   total  = 400;
    const int   slices = 4;         // 每片 = 100

    Twap twap(gw, 1, Side::BUY, total, 20'000'000ULL, slices, /*jitter=*/0.0);
    twap.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (!twap.is_done() && std::chrono::steady_clock::now() < deadline) {
        twap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(twap.is_done());
    for (Qty q : gw.orders()) {
        EXPECT_EQ(q, Qty{100}) << "每片应为 100 手（无 jitter）";
    }
}

TEST(TwapTest, JitterKeepsQtyWithinExpectedRange) {
    MockGateway gw;
    const Qty   total        = 1000;
    const int   slices       = 5;
    const double jitter_ratio = 0.1;    // ±10%
    const Qty   base_qty     = total / slices;  // = 200

    Twap twap(gw, 1, Side::BUY, total, 30'000'000ULL, slices, jitter_ratio);
    twap.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    while (!twap.is_done() && std::chrono::steady_clock::now() < deadline) {
        twap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(twap.is_done());

    // 非最后片：qty ∈ [base*(1-ratio), base*(1+ratio)]
    // 最后片：qty = 剩余量，不检查 jitter 范围
    const auto& orders = gw.orders();
    ASSERT_EQ(orders.size(), static_cast<size_t>(slices));

    for (int i = 0; i < slices - 1; ++i) {
        const Qty lo = static_cast<Qty>(static_cast<double>(base_qty) * (1.0 - jitter_ratio)) - 1;
        const Qty hi = static_cast<Qty>(static_cast<double>(base_qty) * (1.0 + jitter_ratio)) + 1;
        EXPECT_GE(orders[i], lo) << "slice " << i << " qty below lower bound";
        EXPECT_LE(orders[i], hi) << "slice " << i << " qty above upper bound";
    }
    // 总量精确
    EXPECT_EQ(gw.total_qty(), total);
}

TEST(TwapTest, RemainingQtyDecreasesAfterEachTick) {
    MockGateway gw;
    const Qty total = 400;
    Twap twap(gw, 1, Side::BUY, total, 20'000'000ULL, 4, 0.0);
    twap.start();

    Qty prev_remaining = twap.remaining_qty();
    EXPECT_EQ(prev_remaining, total);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (!twap.is_done() && std::chrono::steady_clock::now() < deadline) {
        if (twap.tick()) {
            EXPECT_LT(twap.remaining_qty(), prev_remaining);
            prev_remaining = twap.remaining_qty();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(twap.remaining_qty(), Qty{0});
}

// =================================================================
// Vwap 测试
// =================================================================

TEST(VwapTest, TotalQtyMatchesAfterAllSlices) {
    MockGateway gw;
    const double profile[] = {0.2, 0.3, 0.5};
    const Qty    total     = 1000;
    const int    slices    = 3;

    Vwap vwap(gw, 1, Side::BUY, total, profile, slices, 30'000'000ULL);
    vwap.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (!vwap.is_done() && std::chrono::steady_clock::now() < deadline) {
        vwap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(vwap.is_done());
    EXPECT_EQ(gw.total_qty(), total);
    EXPECT_EQ(gw.order_count(), static_cast<size_t>(slices));
}

TEST(VwapTest, VolumeProfileDeterminesSliceQty) {
    MockGateway gw;
    // 均匀分布：每片 25%
    const double profile[] = {0.25, 0.25, 0.25, 0.25};
    const Qty    total     = 400;
    const int    slices    = 4;

    Vwap vwap(gw, 1, Side::BUY, total, profile, slices, 20'000'000ULL);
    vwap.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (!vwap.is_done() && std::chrono::steady_clock::now() < deadline) {
        vwap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(vwap.is_done());
    ASSERT_EQ(gw.orders().size(), static_cast<size_t>(slices));

    // 每片应为 100 手（400 × 0.25）
    for (Qty q : gw.orders()) {
        EXPECT_EQ(q, Qty{100});
    }
    EXPECT_EQ(gw.total_qty(), total);
}

TEST(VwapTest, SkewedProfileAllocatesMoreToHigherWeightSlice) {
    MockGateway gw;
    const double profile[] = {0.1, 0.9};
    const Qty    total     = 100;
    const int    slices    = 2;

    Vwap vwap(gw, 1, Side::BUY, total, profile, slices, 10'000'000ULL);
    vwap.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (!vwap.is_done() && std::chrono::steady_clock::now() < deadline) {
        vwap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(vwap.is_done());
    const auto& orders = gw.orders();
    ASSERT_EQ(orders.size(), 2u);

    // 第一片 ≈ 10，第二片 ≈ 90
    EXPECT_EQ(orders[0], Qty{10});
    EXPECT_EQ(orders[1], Qty{90});
    EXPECT_EQ(gw.total_qty(), total);
}

TEST(VwapTest, OnFillUpdatesFilledCounter) {
    MockGateway gw;
    const double profile[] = {0.5, 0.5};
    Vwap vwap(gw, 1, Side::BUY, 200, profile, 2, 10'000'000ULL);
    vwap.start();

    // 模拟成交回报不影响 tick() 正常运行
    vwap.on_fill(50);
    vwap.on_fill(50);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (!vwap.is_done() && std::chrono::steady_clock::now() < deadline) {
        vwap.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(vwap.is_done());
    EXPECT_EQ(gw.total_qty(), Qty{200});
}

// =================================================================
// Pov 测试
// =================================================================

TEST(PovTest, ParticipationRateControlsOrderQty) {
    MockGateway gw;
    // participation = 0.1（10%）；total_qty = 1000
    Pov pov(gw, 1, Side::BUY, 1000, 0.1);

    // 市场成交 500 → 目标参与 = 50，已发 = 0，缺口 = 50 > 1 → 发单 50
    pov.on_market_trade(500);
    EXPECT_EQ(gw.order_count(), 1u);
    EXPECT_EQ(gw.last_qty(), Qty{50});
}

TEST(PovTest, FillUpdateSentQtyAndIsDone) {
    MockGateway gw;
    Pov pov(gw, 1, Side::BUY, 100, 0.5);  // 参与率 50%

    pov.on_market_trade(200);  // 目标 = 100，发单 100
    EXPECT_FALSE(pov.is_done());

    pov.on_fill(100);
    EXPECT_TRUE(pov.is_done());
}

TEST(PovTest, NoOrderWhenGapBelowMinSize) {
    MockGateway gw;
    Pov pov(gw, 1, Side::BUY, 1000, 0.01);  // 1% 参与率

    // 市场成交 50 → 目标 = 0.5 → gap = 0 ≤ 1 → 不发单
    pov.on_market_trade(50);
    EXPECT_EQ(gw.order_count(), 0u);
}

TEST(PovTest, AccumulatesMarketVolumeAcrossCalls) {
    MockGateway gw;
    Pov pov(gw, 1, Side::BUY, 1000, 0.1);

    // 分批成交：累计 300，目标 = 30
    pov.on_market_trade(100);  // market=100, target=10, send 10
    pov.on_fill(10);
    pov.on_market_trade(100);  // market=200, target=20, sent=10, gap=10, send 10
    pov.on_fill(10);
    pov.on_market_trade(100);  // market=300, target=30, sent=20, gap=10, send 10
    pov.on_fill(10);

    EXPECT_EQ(gw.order_count(), 3u);
    EXPECT_EQ(gw.total_qty(), Qty{30});
}

TEST(PovTest, StopsOrderingAfterTotalQtyReached) {
    MockGateway gw;
    Pov pov(gw, 1, Side::BUY, 50, 0.1);

    pov.on_market_trade(500);  // target=50，发单 50
    EXPECT_EQ(gw.order_count(), 1u);
    EXPECT_EQ(gw.total_qty(), Qty{50});

    // 再次触发市场成交：ordered_qty_ 已达 total_qty_，不应再发单
    pov.on_market_trade(1000);
    EXPECT_EQ(gw.order_count(), 1u);
}

// =================================================================
// ExecutionEngine 测试
// =================================================================

TEST(ExecutionEngineTest, SubmitRoutesToMinRttGateway) {
    MockGateway fast(true, 200, "fast");
    MockGateway slow(true, 900, "slow");

    ExecutionEngine engine({&slow, &fast});
    engine.submit(make_req());
    engine.submit(make_req());

    EXPECT_EQ(fast.order_count(), 2u);
    EXPECT_EQ(slow.order_count(), 0u);
}

TEST(ExecutionEngineTest, RouterAccessorReturnsSameRouter) {
    MockGateway gw;
    ExecutionEngine engine({&gw});
    SmartRouter& r = engine.router();
    EXPECT_EQ(r.dropped_count(), 0u);
}

TEST(ExecutionEngineTest, DroppedCountIncrementsWhenAllDisconnected) {
    MockGateway gw(false, 500);
    ExecutionEngine engine({&gw});

    engine.submit(make_req());
    engine.submit(make_req());

    EXPECT_EQ(engine.router().dropped_count(), 2u);
}

} // namespace
} // namespace hft
