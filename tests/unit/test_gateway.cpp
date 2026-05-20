// tests/unit/test_gateway.cpp
// 交易所连接层单元测试：IGateway 接口 / SimGateway / SessionManager

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include "connectivity/gateway.hpp"
#include "connectivity/session_manager.hpp"
#include "connectivity/sim_gateway.hpp"
#include "oms/order.hpp"

namespace hft {
namespace {

// ─────────────────────────────────────────────────────────────────
// 辅助：构造一个最简单的 OrderRequest（LIMIT 买单）
// ─────────────────────────────────────────────────────────────────
static OrderRequest make_order(InstrumentId id = 1,
                               Price price     = 10000,
                               Qty   qty       = 100) noexcept {
    OrderRequest r{};
    r.instrument_id   = id;
    r.side            = Side::BUY;
    r.type            = OrderType::LIMIT;
    r.price           = price;
    r.qty             = qty;
    r.strategy_ts_ns  = 0;
    return r;
}

// =================================================================
// SimGateway 基本功能测试
// =================================================================

TEST(SimGatewayTest, InitialOrderCountIsZero) {
    SimGateway gw;
    EXPECT_EQ(gw.order_count(), 0u);
}

TEST(SimGatewayTest, SendNewOrderIncrementsOrderCount) {
    SimGateway gw;
    gw.send_new_order(make_order());
    EXPECT_EQ(gw.order_count(), 1u);

    gw.send_new_order(make_order(2, 20000, 50));
    EXPECT_EQ(gw.order_count(), 2u);
}

TEST(SimGatewayTest, FillProbabilityOneAlwaysFills) {
    SimGateway gw(/*fill_probability=*/1.0);

    const int N = 20;
    for (int i = 0; i < N; ++i) {
        gw.send_new_order(make_order());
    }
    EXPECT_EQ(gw.order_count(), static_cast<size_t>(N));

    int fills = 0;
    FillEvent ev{};
    while (gw.pop_fill(ev)) { ++fills; }
    EXPECT_EQ(fills, N);
}

TEST(SimGatewayTest, FillProbabilityZeroNeverFills) {
    SimGateway gw(/*fill_probability=*/0.0);

    const int N = 20;
    for (int i = 0; i < N; ++i) {
        gw.send_new_order(make_order());
    }
    EXPECT_EQ(gw.order_count(), static_cast<size_t>(N));

    FillEvent ev{};
    EXPECT_FALSE(gw.pop_fill(ev));
}

TEST(SimGatewayTest, PopFillReturnsFalseWhenEmpty) {
    SimGateway gw;
    FillEvent ev{};
    EXPECT_FALSE(gw.pop_fill(ev));
}

TEST(SimGatewayTest, FillEventFieldsMatchOrderRequest) {
    SimGateway gw(1.0, /*ack_latency_ns=*/5000);

    OrderRequest req = make_order(/*id=*/7, /*price=*/99900, /*qty=*/42);
    gw.send_new_order(req);

    FillEvent ev{};
    ASSERT_TRUE(gw.pop_fill(ev));

    EXPECT_EQ(ev.instrument_id, req.instrument_id);
    EXPECT_EQ(ev.side,          req.side);
    EXPECT_EQ(ev.fill_price,    req.price);
    EXPECT_EQ(ev.fill_qty,      req.qty);
    EXPECT_EQ(ev.remaining_qty, static_cast<Qty>(0));
    // exchange_order_id は 1 から始まる単調増加
    EXPECT_GE(ev.exchange_order_id, static_cast<uint64_t>(1));
}

TEST(SimGatewayTest, MultipleOrdersFillEventsAreSequential) {
    SimGateway gw(1.0);

    gw.send_new_order(make_order(1));
    gw.send_new_order(make_order(2));

    FillEvent e1{}, e2{};
    ASSERT_TRUE(gw.pop_fill(e1));
    ASSERT_TRUE(gw.pop_fill(e2));
    EXPECT_LT(e1.exchange_order_id, e2.exchange_order_id);
}

TEST(SimGatewayTest, IsConnectedDefaultTrue) {
    SimGateway gw;
    EXPECT_TRUE(gw.is_connected());
}

TEST(SimGatewayTest, SetConnectedToggle) {
    SimGateway gw;
    gw.set_connected(false);
    EXPECT_FALSE(gw.is_connected());
    gw.set_connected(true);
    EXPECT_TRUE(gw.is_connected());
}

TEST(SimGatewayTest, GetRttNsReturnsConstructedValue) {
    SimGateway gw(1.0, /*ack_latency_ns=*/12345u);
    EXPECT_EQ(gw.get_rtt_ns(), 12345u);
}

TEST(SimGatewayTest, NameIsSimGateway) {
    SimGateway gw;
    EXPECT_EQ(gw.name(), "SimGateway");
}

TEST(SimGatewayTest, SendCancelIsNoop) {
    SimGateway gw(1.0);
    gw.send_new_order(make_order());
    // 撤单不应崩溃，也不修改已有报单数
    gw.send_cancel(999);
    EXPECT_EQ(gw.order_count(), 1u);
}

TEST(SimGatewayTest, SendModifyIsNoop) {
    SimGateway gw(1.0);
    gw.send_new_order(make_order());
    ModifyRequest mr{};
    mr.client_order_id = 0;
    mr.new_price       = 20000;
    mr.new_qty         = 50;
    gw.send_modify(mr);
    EXPECT_EQ(gw.order_count(), 1u);
}

// =================================================================
// IGateway 多态接口测试
// =================================================================

TEST(IGatewayTest, SimGatewayAccessibleViaPointer) {
    SimGateway sim(1.0);
    IGateway* gw = &sim;

    OrderRequest req = make_order(3, 5000, 10);
    gw->send_new_order(req);

    EXPECT_TRUE(gw->is_connected());
    EXPECT_EQ(gw->get_rtt_ns(), 1000u);  // 默认值
    EXPECT_EQ(gw->name(), "SimGateway");

    // 通过基类指针发送的报单应在 SimGateway 中可见
    EXPECT_EQ(sim.order_count(), 1u);
    FillEvent ev{};
    EXPECT_TRUE(sim.pop_fill(ev));
}

// =================================================================
// SessionManager 测试
// =================================================================

// 心跳间隔设为 50ms，使测试能在 300ms 内可靠完成
static constexpr uint32_t kTestHbMs = 50u;

TEST(SessionManagerTest, InitialStateIsDisconnected) {
    SimGateway     gw;
    SessionManager sm(gw, kTestHbMs);
    EXPECT_EQ(sm.state(), SessionManager::State::DISCONNECTED);
}

TEST(SessionManagerTest, AfterStartWithConnectedGatewayStateBecomesConnected) {
    SimGateway     gw;   // is_connected() = true
    SessionManager sm(gw, kTestHbMs);

    sm.start();
    // 等待心跳线程至少跑一次（心跳间隔 50ms，等 200ms 足够）
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(sm.state(), SessionManager::State::CONNECTED);
    sm.stop();
}

TEST(SessionManagerTest, DisconnectionTriggersReconnecting) {
    SimGateway     gw;   // 初始连接
    SessionManager sm(gw, kTestHbMs);

    sm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(sm.state(), SessionManager::State::CONNECTED);

    // 模拟断连
    gw.set_connected(false);
    // 等待心跳检测到断连（最多两个心跳周期）
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(sm.state(), SessionManager::State::RECONNECTING);
    EXPECT_GT(sm.reconnect_count(), static_cast<uint64_t>(0));
    sm.stop();
}

TEST(SessionManagerTest, ReconnectCountIncreasesWhileDisconnected) {
    SimGateway     gw;
    SessionManager sm(gw, kTestHbMs);

    gw.set_connected(false);  // 启动前就断开
    sm.start();

    // 退避从 100ms 开始，等 500ms 应至少有 2 次重试
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_GE(sm.reconnect_count(), static_cast<uint64_t>(2));
    sm.stop();
}

TEST(SessionManagerTest, StopIsIdempotent) {
    SimGateway     gw;
    SessionManager sm(gw, kTestHbMs);

    sm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sm.stop();  // 第一次 stop
    sm.stop();  // 第二次 stop 不应崩溃
    EXPECT_EQ(sm.state(), SessionManager::State::DISCONNECTED);
}

TEST(SessionManagerTest, StartIsIdempotent) {
    SimGateway     gw;
    SessionManager sm(gw, kTestHbMs);

    sm.start();
    sm.start();  // 重复 start 不应崩溃或启动第二个线程
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sm.stop();
}

TEST(SessionManagerTest, ReconnectCountIsZeroInitially) {
    SimGateway     gw;
    SessionManager sm(gw, kTestHbMs);
    EXPECT_EQ(sm.reconnect_count(), 0u);
}

TEST(SessionManagerTest, StateReturnsDisconnectedAfterStop) {
    SimGateway     gw;
    SessionManager sm(gw, kTestHbMs);

    sm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sm.stop();

    EXPECT_EQ(sm.state(), SessionManager::State::DISCONNECTED);
}

} // namespace
} // namespace hft
