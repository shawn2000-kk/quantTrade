// tests/unit/test_oms.cpp
// GoogleTest 单元测试：OMS 层
//
// 编译示例（手动，无 CMake）：
//   c++ -std=c++20 -I src \
//       tests/unit/test_oms.cpp \
//       src/oms/order_state_machine.cpp \
//       src/oms/fill_tracker.cpp \
//       src/oms/oms.cpp \
//       -lgtest -lgtest_main -o test_oms && ./test_oms

#include <gtest/gtest.h>
#include <memory>
#include <cstdint>

#include "oms/order.hpp"
#include "oms/order_state_machine.hpp"
#include "oms/fill_tracker.hpp"
#include "oms/oms.hpp"

namespace hft {

// ── 辅助：构造合法的 OrderRequest ────────────────────────────────
static OrderRequest make_req(InstrumentId id   = 42,
                             Price        price = 10000,
                             Qty          qty   = 100) noexcept {
    OrderRequest req{};
    req.instrument_id  = id;
    req.side           = Side::BUY;
    req.type           = OrderType::LIMIT;
    req.price          = price;
    req.qty            = qty;
    req.strategy_ts_ns = 0;
    return req;
}

// 构造合法的 OrderAck（NEW）
static OrderAck make_ack(uint64_t cloid,
                         uint64_t exoid     = 9001,
                         OrderStatus status = OrderStatus::NEW) noexcept {
    OrderAck ack{};
    ack.client_order_id   = cloid;
    ack.exchange_order_id = exoid;
    ack.status            = status;
    ack.ack_ts_ns         = 111'000'000ULL;
    return ack;
}

// 构造成交回报
static FillEvent make_fill(uint64_t cloid,
                           InstrumentId id,
                           Qty      fill_qty,
                           Qty      remaining) noexcept {
    FillEvent f{};
    f.client_order_id   = cloid;
    f.exchange_order_id = 9001;
    f.instrument_id     = id;
    f.side              = Side::BUY;
    f.fill_price        = 10000;
    f.fill_qty          = fill_qty;
    f.remaining_qty     = remaining;
    f.fill_ts_ns        = 222'000'000ULL;
    return f;
}

// ── 测试夹具 ─────────────────────────────────────────────────────
// OMS 内含 4 MB MemoryPool，用 unique_ptr 避免大栈帧
class OMSTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = std::make_unique<FillTracker>();
        oms_     = std::make_unique<OMS>(*tracker_);
    }

    void TearDown() override {
        oms_.reset();
        tracker_.reset();
    }

    std::unique_ptr<FillTracker> tracker_;
    std::unique_ptr<OMS>         oms_;
};

// ═══════════════════════════════════════════════════════════════════
// 1. 完整生命周期：submit → ACK → 部分成交 → 全部成交
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, FullLifecyclePartialThenFull) {
    const OrderRequest req = make_req(42, 10000, 100);

    // submit: 状态 = PENDING_NEW，client_order_id >= 1
    Order* order = oms_->submit(req);
    ASSERT_NE(order, nullptr);
    const uint64_t cloid = order->client_order_id;
    EXPECT_GE(cloid, 1u);
    EXPECT_EQ(order->status,       OrderStatus::PENDING_NEW);
    EXPECT_EQ(order->filled_qty,   0);
    EXPECT_EQ(order->remaining_qty(), 100);
    EXPECT_EQ(oms_->active_orders(), 1u);

    // on_ack: 状态 → NEW
    EXPECT_TRUE(oms_->on_ack(make_ack(cloid, 9001)));
    EXPECT_EQ(order->status,             OrderStatus::NEW);
    EXPECT_EQ(order->exchange_order_id,  9001u);
    EXPECT_EQ(order->ack_ts_ns,          111'000'000ULL);

    // 部分成交 40 手：状态 → PARTIALLY_FILLED
    EXPECT_TRUE(oms_->on_fill(make_fill(cloid, 42, 40, 60)));
    EXPECT_EQ(order->status,      OrderStatus::PARTIALLY_FILLED);
    EXPECT_EQ(order->filled_qty,  40);
    EXPECT_EQ(order->remaining_qty(), 60);

    // 剩余 60 手全部成交：状态 → FILLED
    EXPECT_TRUE(oms_->on_fill(make_fill(cloid, 42, 60, 0)));
    EXPECT_EQ(order->status,      OrderStatus::FILLED);
    EXPECT_EQ(order->filled_qty,  100);
    EXPECT_EQ(order->remaining_qty(), 0);
    EXPECT_TRUE(order->is_terminal());

    // FillTracker 应收到 2 笔成交
    EXPECT_EQ(tracker_->total_fills(), 2u);
    EXPECT_EQ(tracker_->total_filled_qty(42), 100);

    // release 后订单从 map 移除
    oms_->release(cloid);
    EXPECT_EQ(oms_->active_orders(), 0u);
    EXPECT_EQ(oms_->find(cloid), nullptr);
}

// ═══════════════════════════════════════════════════════════════════
// 2. 非法状态转换：FILLED → NEW 应被拒绝
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, IllegalTransitionFromTerminal) {
    Order* order = oms_->submit(make_req());
    ASSERT_NE(order, nullptr);
    const uint64_t cloid = order->client_order_id;

    // ACK → NEW
    ASSERT_TRUE(oms_->on_ack(make_ack(cloid, 9002)));
    // 一次全部成交 → FILLED
    ASSERT_TRUE(oms_->on_fill(make_fill(cloid, 42, 100, 0)));
    ASSERT_EQ(order->status, OrderStatus::FILLED);

    // 尝试对已 FILLED 的订单再次 ACK（非法）
    OrderAck bad_ack = make_ack(cloid, 9002);
    bad_ack.status = OrderStatus::NEW;
    EXPECT_FALSE(oms_->on_ack(bad_ack));
    // 状态不应改变
    EXPECT_EQ(order->status, OrderStatus::FILLED);

    // 直接调用 transition：FILLED → NEW 非法
    EXPECT_FALSE(OrderStateMachine::transition(*order, OrderStatus::NEW));
    EXPECT_EQ(order->status, OrderStatus::FILLED);

    // FILLED → REJECTED 也非法
    EXPECT_FALSE(OrderStateMachine::transition(*order, OrderStatus::REJECTED));
    EXPECT_EQ(order->status, OrderStatus::FILLED);
}

// ═══════════════════════════════════════════════════════════════════
// 3. REJECTED ACK：状态机正确转换到 REJECTED
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, AckRejected) {
    Order* order = oms_->submit(make_req());
    ASSERT_NE(order, nullptr);
    const uint64_t cloid = order->client_order_id;

    // 发送 REJECTED ACK
    OrderAck rej = make_ack(cloid, 0, OrderStatus::REJECTED);
    EXPECT_TRUE(oms_->on_ack(rej));
    EXPECT_EQ(order->status, OrderStatus::REJECTED);
    EXPECT_TRUE(order->is_terminal());

    // REJECTED 后不可再成交
    EXPECT_FALSE(oms_->on_fill(make_fill(cloid, 42, 10, 90)));
    EXPECT_EQ(order->filled_qty, 0);  // 不变

    // REJECTED 后不可再 cancel
    EXPECT_FALSE(oms_->on_cancel_ack(cloid));
}

// ═══════════════════════════════════════════════════════════════════
// 4. 撤单流程：submit → ACK → cancel_request → cancel_ack
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, CancelFlow) {
    Order* order = oms_->submit(make_req(10, 5000, 50));
    ASSERT_NE(order, nullptr);
    const uint64_t cloid = order->client_order_id;

    // ACK → NEW
    ASSERT_TRUE(oms_->on_ack(make_ack(cloid, 8888)));
    ASSERT_EQ(order->status, OrderStatus::NEW);

    // 模拟策略发出撤单请求：手动转换到 PENDING_CANCEL
    EXPECT_TRUE(OrderStateMachine::transition(*order, OrderStatus::PENDING_CANCEL));
    EXPECT_EQ(order->status, OrderStatus::PENDING_CANCEL);
    EXPECT_TRUE(order->is_active());

    // 交易所确认撤单
    EXPECT_TRUE(oms_->on_cancel_ack(cloid));
    EXPECT_EQ(order->status, OrderStatus::CANCELLED);
    EXPECT_TRUE(order->is_terminal());

    // 撤单后不可再成交
    EXPECT_FALSE(OrderStateMachine::apply_fill(*order, 10));

    // FillTracker 不应有成交
    EXPECT_EQ(tracker_->total_fills(), 0u);

    oms_->release(cloid);
    EXPECT_EQ(oms_->active_orders(), 0u);
}

// ═══════════════════════════════════════════════════════════════════
// 4b. PENDING_CANCEL → FILLED（撤单确认前全部成交）
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, PendingCancelThenFilled) {
    Order* order = oms_->submit(make_req(5, 2000, 200));
    ASSERT_NE(order, nullptr);
    const uint64_t cloid = order->client_order_id;

    ASSERT_TRUE(oms_->on_ack(make_ack(cloid, 7777)));
    ASSERT_TRUE(OrderStateMachine::transition(*order, OrderStatus::PENDING_CANCEL));

    // 撤单前全部成交
    EXPECT_TRUE(oms_->on_fill(make_fill(cloid, 5, 200, 0)));
    EXPECT_EQ(order->status, OrderStatus::FILLED);

    // 此后撤单 ACK 应被拒绝（已 terminal）
    EXPECT_FALSE(oms_->on_cancel_ack(cloid));
}

// ═══════════════════════════════════════════════════════════════════
// 5. release 后内存池可重用：连续提交 1000 笔订单无 nullptr
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, MemoryPoolReuse) {
    constexpr int ROUNDS = 1000;

    for (int i = 0; i < ROUNDS; ++i) {
        OrderRequest req = make_req(static_cast<InstrumentId>(i % 64),
                                    static_cast<Price>(1000 + i),
                                    static_cast<Qty>(10));
        Order* order = oms_->submit(req);
        ASSERT_NE(order, nullptr) << "submit returned nullptr at iteration " << i;

        const uint64_t cloid = order->client_order_id;

        // ACK → NEW
        ASSERT_TRUE(oms_->on_ack(make_ack(cloid, static_cast<uint64_t>(9000 + i))));
        // 全部成交 → FILLED
        ASSERT_TRUE(oms_->on_fill(make_fill(cloid,
                                            static_cast<InstrumentId>(i % 64),
                                            10, 0)));
        ASSERT_EQ(order->status, OrderStatus::FILLED);

        // 释放归还内存池，供下一轮复用
        oms_->release(cloid);
    }

    // 全部释放后，map 应为空
    EXPECT_EQ(oms_->active_orders(), 0u);
}

// ═══════════════════════════════════════════════════════════════════
// 6. FillTracker：pop_fill 能取出所有入队成交
// ═══════════════════════════════════════════════════════════════════
TEST_F(OMSTest, FillTrackerPopAll) {
    constexpr int N = 10;

    // 提交 N 笔订单，每笔全部成交
    for (int i = 0; i < N; ++i) {
        OrderRequest req = make_req(static_cast<InstrumentId>(i), 100 * (i + 1), 50);
        Order* order = oms_->submit(req);
        ASSERT_NE(order, nullptr);

        const uint64_t cloid = order->client_order_id;
        ASSERT_TRUE(oms_->on_ack(make_ack(cloid, static_cast<uint64_t>(5000 + i))));
        ASSERT_TRUE(oms_->on_fill(make_fill(cloid,
                                            static_cast<InstrumentId>(i),
                                            50, 0)));
        oms_->release(cloid);
    }

    // 确认统计
    EXPECT_EQ(tracker_->total_fills(), static_cast<uint64_t>(N));

    // 弹出所有成交
    int popped = 0;
    FillEvent ev{};
    while (tracker_->pop_fill(ev)) {
        ++popped;
        // 验证成交数量均为 50
        EXPECT_EQ(ev.fill_qty, 50);
    }
    EXPECT_EQ(popped, N);

    // 队列已空
    EXPECT_FALSE(tracker_->pop_fill(ev));

    // 品种级别统计（每种各 50 手）
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(tracker_->total_filled_qty(static_cast<InstrumentId>(i)), 50)
            << "InstrumentId " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════
// OrderStateMachine 独立测试
// ═══════════════════════════════════════════════════════════════════

TEST(OrderStateMachineTest, StatusNames) {
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::PENDING_NEW),      "PENDING_NEW");
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::NEW),               "NEW");
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::PARTIALLY_FILLED),  "PARTIALLY_FILLED");
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::FILLED),            "FILLED");
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::PENDING_CANCEL),    "PENDING_CANCEL");
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::CANCELLED),         "CANCELLED");
    EXPECT_STREQ(OrderStateMachine::status_name(OrderStatus::REJECTED),          "REJECTED");
}

TEST(OrderStateMachineTest, ApplyFillOverfill) {
    // overfill：fill_qty 超过 remaining，应钳位到 FILLED
    Order order{};
    order.qty         = 100;
    order.filled_qty  = 0;
    order.status      = OrderStatus::NEW;

    EXPECT_TRUE(OrderStateMachine::apply_fill(order, 150));  // 超额成交
    EXPECT_EQ(order.status,     OrderStatus::FILLED);
    EXPECT_EQ(order.filled_qty, 100);                        // 钳位到 qty
}

TEST(OrderStateMachineTest, ApplyFillOnTerminalReturnsfalse) {
    Order order{};
    order.qty        = 100;
    order.filled_qty = 100;
    order.status     = OrderStatus::FILLED;

    EXPECT_FALSE(OrderStateMachine::apply_fill(order, 10));
    EXPECT_EQ(order.filled_qty, 100);  // 不变
}

TEST(OrderStateMachineTest, ApplyFillZeroQtyReturnsFalse) {
    Order order{};
    order.qty        = 100;
    order.filled_qty = 0;
    order.status     = OrderStatus::NEW;

    EXPECT_FALSE(OrderStateMachine::apply_fill(order, 0));
    EXPECT_EQ(order.status, OrderStatus::NEW);  // 不变
}

TEST(OrderStateMachineTest, PendingCancelPartialFillRejected) {
    // PENDING_CANCEL 时仅允许全部成交（→ FILLED），不允许部分成交
    Order order{};
    order.qty        = 100;
    order.filled_qty = 0;
    order.status     = OrderStatus::PENDING_CANCEL;

    EXPECT_FALSE(OrderStateMachine::apply_fill(order, 50));  // 非法：PENDING_CANCEL→PARTIALLY_FILLED
    EXPECT_EQ(order.status,     OrderStatus::PENDING_CANCEL);
    EXPECT_EQ(order.filled_qty, 0);

    // 全部成交应该被允许
    EXPECT_TRUE(OrderStateMachine::apply_fill(order, 100));
    EXPECT_EQ(order.status, OrderStatus::FILLED);
}

// ═══════════════════════════════════════════════════════════════════
// find / release 语义
// ═══════════════════════════════════════════════════════════════════

TEST_F(OMSTest, FindAndRelease) {
    Order* order = oms_->submit(make_req());
    ASSERT_NE(order, nullptr);
    const uint64_t cloid = order->client_order_id;

    EXPECT_EQ(oms_->find(cloid), order);
    EXPECT_EQ(oms_->find(cloid + 999), nullptr);

    oms_->release(cloid);
    EXPECT_EQ(oms_->find(cloid), nullptr);

    // release 不存在的 ID 应安全（no-op）
    oms_->release(cloid);  // 二次释放：不崩溃
}

TEST_F(OMSTest, SubmitReturnsNullWhenPoolExhausted) {
    // 提交 65536 笔不释放，第 65537 笔应返回 nullptr
    constexpr size_t POOL_SIZE = 65536;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        Order* o = oms_->submit(make_req());
        ASSERT_NE(o, nullptr) << "Unexpected nullptr at iteration " << i;
    }
    // 池满
    EXPECT_EQ(oms_->submit(make_req()), nullptr);
}

} // namespace hft
