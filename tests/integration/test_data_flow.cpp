// tests/integration/test_data_flow.cpp
// 端到端链路集成测试：验证一条 BBOEvent 从行情层流经策略、风控、OMS、网关到持仓的完整路径

#include <gtest/gtest.h>
#include <cstdio>

#include "market_data/market_data_types.hpp"
#include "strategy/market_maker.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/rate_limiter.hpp"
#include "risk/position_limits.hpp"
#include "risk/pre_trade_risk.hpp"
#include "oms/oms.hpp"
#include "oms/fill_tracker.hpp"
#include "oms/order_state_machine.hpp"
#include "connectivity/sim_gateway.hpp"
#include "position/position_manager.hpp"

namespace hft {

// ── 测试夹具：组装完整链路 ────────────────────────────────────────────
class DataFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        cb_   = std::make_unique<CircuitBreaker>(-50'000);
        rl_   = std::make_unique<RateLimiter>(1000, 2000);
        pl_   = std::make_unique<PositionLimits>(1000, 100'000'000LL);
        risk_ = std::make_unique<PreTradeRisk>(*cb_, *rl_, *pl_);

        fill_tracker_ = std::make_unique<FillTracker>();
        oms_          = std::make_unique<OMS>(*fill_tracker_);

        // fill_probability=1.0（每笔必成交），ack_latency_ns=0（零延迟）
        sim_gw_ = std::make_unique<SimGateway>(1.0, 0);

        // instrument=1, spread=4tick, qty=100手, max_inventory=500手
        mm_ = std::make_unique<MarketMaker>(1, 4, 100, 500);

        pos_mgr_ = std::make_unique<PositionManager>();
    }

    // 辅助：把策略产出的所有报单跑完整链路，返回成功提交数
    //
    // OMS 状态机要求：PENDING_NEW → NEW（ACK）→ FILLED（成交）
    // SimGateway 只生成 FillEvent，不生成 ACK，测试层需手动补 ACK。
    // 同时 SimGateway 的 FillEvent.client_order_id == 0，需用 OMS 分配的 cloid 回填。
    int drain_orders() {
        int submitted = 0;
        OrderRequest req{};
        while (mm_->pop_order(req)) {
            if (!risk_->check(req)) continue;

            Order* order = oms_->submit(req);
            if (!order) continue;
            const uint64_t cloid = order->client_order_id;

            // ACK：PENDING_NEW → NEW
            OrderAck ack{};
            ack.client_order_id   = cloid;
            ack.exchange_order_id = cloid + 9000;
            ack.status            = OrderStatus::NEW;
            oms_->on_ack(ack);

            sim_gw_->send_new_order(req);
            ++submitted;

            // 消费成交回报，回填 cloid 后交给 OMS
            FillEvent fill{};
            while (sim_gw_->pop_fill(fill)) {
                fill.client_order_id = cloid;
                oms_->on_fill(fill);
            }

            // 消费 FillTracker，更新持仓
            FillEvent ft_fill{};
            while (fill_tracker_->pop_fill(ft_fill)) {
                pos_mgr_->on_fill(ft_fill);
            }

            // 终态订单从 OMS 活跃 map 移除
            if (order->is_terminal()) {
                oms_->release(cloid);
            }
        }
        return submitted;
    }

    std::unique_ptr<CircuitBreaker>  cb_;
    std::unique_ptr<RateLimiter>     rl_;
    std::unique_ptr<PositionLimits>  pl_;
    std::unique_ptr<PreTradeRisk>    risk_;
    std::unique_ptr<FillTracker>     fill_tracker_;
    std::unique_ptr<OMS>             oms_;
    std::unique_ptr<SimGateway>      sim_gw_;
    std::unique_ptr<MarketMaker>     mm_;
    std::unique_ptr<PositionManager> pos_mgr_;
};

// ── 构造标准 BBO 行情事件 ─────────────────────────────────────────────
static BBOEvent make_bbo(Price bid, Price ask, InstrumentId id = 1) noexcept {
    BBOEvent e{};
    e.instrument_id = id;
    e.bid_px  = bid;  e.bid_qty  = 500;
    e.ask_px  = ask;  e.ask_qty  = 500;
    e.type    = EventType::BBO_UPDATE;
    return e;
}

// ═══════════════════════════════════════════════════════════════════════
// 1. 基础链路：一条 BBO → 策略双边报单 → 风控通过 → OMS 登记 → 成交 → 持仓更新
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DataFlowTest, BasicBboToPosition) {
    // [1] 行情进入策略
    mm_->on_bbo(make_bbo(9998, 10002));

    // [2] 策略应产出 2 笔报单（BUY + SELL 双边做市）
    int submitted = drain_orders();
    EXPECT_EQ(submitted, 2);

    // [3] SimGateway 收到报单
    EXPECT_EQ(sim_gw_->order_count(), 2u);

    // [4] 成交后 OMS 活跃订单应为 0（已全部 FILLED）
    EXPECT_EQ(oms_->active_orders(), 0u);

    // [5] FillTracker 记录了 2 笔成交
    EXPECT_EQ(fill_tracker_->total_fills(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════
// 2. 风控拦截：熔断器打开后报单被拒
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DataFlowTest, CircuitBreakerBlocksOrders) {
    // 触发熔断：亏损超过 daily_loss_limit(-50000)
    cb_->update_pnl(-60'000);
    ASSERT_FALSE(cb_->allow_order());

    mm_->on_bbo(make_bbo(9998, 10002));

    // 风控应拒绝所有报单
    int submitted = drain_orders();
    EXPECT_EQ(submitted, 0);
    EXPECT_EQ(sim_gw_->order_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// 3. 行情不变时策略不重复报单（mid 未移动不触发）
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DataFlowTest, NoDuplicateOrdersOnSameBbo) {
    mm_->on_bbo(make_bbo(9998, 10002));
    int first = drain_orders();
    EXPECT_EQ(first, 2);

    // 发相同行情，mid 未变，策略不应再产出报单
    mm_->on_bbo(make_bbo(9998, 10002));
    int second = drain_orders();
    EXPECT_EQ(second, 0);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. OMS 状态机：报单经过完整生命周期后终态为 FILLED
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DataFlowTest, OrderReachesFilledState) {
    mm_->on_bbo(make_bbo(9998, 10002));

    OrderRequest req{};
    while (mm_->pop_order(req)) {
        ASSERT_TRUE(risk_->check(req));

        Order* order = oms_->submit(req);
        ASSERT_NE(order, nullptr);
        EXPECT_EQ(order->status, OrderStatus::PENDING_NEW);

        // ACK：PENDING_NEW → NEW
        OrderAck ack{};
        ack.client_order_id   = order->client_order_id;
        ack.exchange_order_id = order->client_order_id + 9000;
        ack.status            = OrderStatus::NEW;
        ASSERT_TRUE(oms_->on_ack(ack));
        EXPECT_EQ(order->status, OrderStatus::NEW);

        sim_gw_->send_new_order(req);

        FillEvent fill{};
        ASSERT_TRUE(sim_gw_->pop_fill(fill));
        fill.client_order_id = order->client_order_id;
        oms_->on_fill(fill);

        // NEW → FILLED
        EXPECT_EQ(order->status, OrderStatus::FILLED);
        EXPECT_EQ(order->filled_qty, order->qty);
        EXPECT_TRUE(order->is_terminal());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 5. 持仓方向正确：BUY 成交后净持仓为正，SELL 成交后净持仓减少
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DataFlowTest, PositionDirectionAfterFill) {
    mm_->on_bbo(make_bbo(9998, 10002));

    // 只处理 BUY 报单，验证多头持仓
    OrderRequest req{};
    while (mm_->pop_order(req)) {
        if (req.side != Side::BUY) continue;
        if (!risk_->check(req)) continue;

        Order* order = oms_->submit(req);
        ASSERT_NE(order, nullptr);

        OrderAck ack{};
        ack.client_order_id   = order->client_order_id;
        ack.exchange_order_id = order->client_order_id + 9000;
        ack.status            = OrderStatus::NEW;
        oms_->on_ack(ack);

        sim_gw_->send_new_order(req);

        FillEvent fill{};
        while (sim_gw_->pop_fill(fill)) {
            fill.client_order_id = order->client_order_id;
            oms_->on_fill(fill);
        }
        FillEvent ft_fill{};
        while (fill_tracker_->pop_fill(ft_fill)) {
            pos_mgr_->on_fill(ft_fill);
        }
    }

    // BUY 成交后净持仓 > 0
    EXPECT_GT(pos_mgr_->get_net_qty(1), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. 连续行情：多次 BBO 变化，每次 mid 移动都触发新一轮报单
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DataFlowTest, MultipleBoUpdatesGenerateOrders) {
    const Price spreads[][2] = {
        {9998, 10002},  // mid = 20000 (sum)
        {9996, 10004},  // mid = 20000 → 未变，不触发
        {10000, 10004}, // mid = 20004 → 移动，触发
        {10002, 10006}, // mid = 20008 → 移动，触发
    };

    int total = 0;
    for (auto& s : spreads) {
        mm_->on_bbo(make_bbo(s[0], s[1]));
        total += drain_orders();
    }

    // 第 1、3、4 次 mid 变化（第 2 次 mid 不变），应有 3 轮 × 2 笔 = 6 笔
    EXPECT_EQ(total, 6);
}

} // namespace hft
