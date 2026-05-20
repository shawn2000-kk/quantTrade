#include <gtest/gtest.h>
#include <thread>
#include <chrono>

// 被测模块
#include "risk/circuit_breaker.hpp"
#include "risk/rate_limiter.hpp"
#include "risk/position_limits.hpp"
#include "risk/pre_trade_risk.hpp"

using namespace hft;
using namespace std::chrono_literals;

// ============================================================
// CircuitBreaker 测试
// ============================================================
class CircuitBreakerTest : public ::testing::Test {
protected:
    // open_threshold = -5000（亏损 50 元触发熔断）
    CircuitBreaker cb{-5000};
};

TEST_F(CircuitBreakerTest, InitialStateAllowsOrders) {
    EXPECT_EQ(cb.state(), CBState::CLOSED);
    EXPECT_TRUE(cb.allow_order());
    EXPECT_EQ(cb.daily_pnl(), 0);
}

TEST_F(CircuitBreakerTest, PositivePnlNoBreaker) {
    cb.update_pnl(10000);   // 盈利
    EXPECT_EQ(cb.state(), CBState::CLOSED);
    EXPECT_TRUE(cb.allow_order());
}

TEST_F(CircuitBreakerTest, PnlBelowThresholdTriggersOpen) {
    cb.update_pnl(-3000);
    EXPECT_EQ(cb.state(), CBState::CLOSED);  // 未到阈值
    EXPECT_TRUE(cb.allow_order());

    cb.update_pnl(-2001);   // 累计 -5001，低于 -5000
    EXPECT_EQ(cb.state(), CBState::OPEN);
    EXPECT_FALSE(cb.allow_order());
}

TEST_F(CircuitBreakerTest, ExactThresholdTriggersOpen) {
    cb.update_pnl(-5000);   // 恰好等于阈值 → 触发
    EXPECT_EQ(cb.state(), CBState::OPEN);
    EXPECT_FALSE(cb.allow_order());
}

TEST_F(CircuitBreakerTest, OpenStateRejectsAllOrders) {
    cb.update_pnl(-9999999);
    ASSERT_EQ(cb.state(), CBState::OPEN);

    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(cb.allow_order());
    }
}

TEST_F(CircuitBreakerTest, ResetRestoresClosed) {
    cb.update_pnl(-9999);
    ASSERT_EQ(cb.state(), CBState::OPEN);

    cb.reset();

    EXPECT_EQ(cb.state(), CBState::CLOSED);
    EXPECT_TRUE(cb.allow_order());
    EXPECT_EQ(cb.daily_pnl(), 0);  // PnL 也被清零
}

TEST_F(CircuitBreakerTest, MultipleUpdatesAccumulate) {
    cb.update_pnl(-1000);
    cb.update_pnl(-1000);
    cb.update_pnl(-1000);
    EXPECT_EQ(cb.daily_pnl(), -3000);
    EXPECT_EQ(cb.state(), CBState::CLOSED);

    cb.update_pnl(-2001);
    EXPECT_EQ(cb.state(), CBState::OPEN);
}

// ============================================================
// RateLimiter 测试
// ============================================================
class RateLimiterTest : public ::testing::Test {
protected:
    // rate=10/s, burst=5：桶最多容纳 5 个令牌，每秒补充 10 个
    RateLimiter rl{10, 5};
};

TEST_F(RateLimiterTest, BurstAcquireSucceeds) {
    // 初始令牌 = burst = 5，连续 5 次应全部成功
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rl.try_acquire()) << "acquire #" << i << " failed";
    }
}

TEST_F(RateLimiterTest, ExceedBurstFails) {
    // 消耗完 burst
    for (int i = 0; i < 5; ++i) (void)rl.try_acquire();
    // 第 6 次应失败
    EXPECT_FALSE(rl.try_acquire());
}

TEST_F(RateLimiterTest, WaitAndRefillSucceeds) {
    // 消耗完 burst
    for (int i = 0; i < 5; ++i) (void)rl.try_acquire();
    ASSERT_FALSE(rl.try_acquire());

    // 等待 300ms：rate=10/s → 300ms 产生 3 个令牌
    std::this_thread::sleep_for(300ms);

    // 至少 2 次应成功（给 sleep 精度留余量）
    int success = 0;
    for (int i = 0; i < 3; ++i) {
        if (rl.try_acquire()) ++success;
    }
    EXPECT_GE(success, 2);
}

TEST_F(RateLimiterTest, AcquireMultipleTokens) {
    // 一次消耗 3 个令牌
    EXPECT_TRUE(rl.try_acquire(3));
    // 还剩 2 个令牌，消耗 3 个应失败
    EXPECT_FALSE(rl.try_acquire(3));
    // 消耗 2 个应成功
    EXPECT_TRUE(rl.try_acquire(2));
}

TEST_F(RateLimiterTest, LargeBurstAllowsManyAcquires) {
    // rate=1/s（补充极慢，测试执行期间不会触发任何补充）
    // burst=100：初始令牌充满，连续 100 次应全部成功，第 101 次失败
    RateLimiter slow_rl{1, 100};
    int success = 0;
    for (int i = 0; i < 100; ++i) {
        if (slow_rl.try_acquire()) ++success;
    }
    EXPECT_EQ(success, 100);       // burst 内全部成功
    EXPECT_FALSE(slow_rl.try_acquire());  // 超出 burst，且补充极慢（不可能在微秒内补充）
}

// ============================================================
// PositionLimits 测试
// ============================================================
class PositionLimitsTest : public ::testing::Test {
protected:
    // max_net_qty=100, max_notional=1000000（100 万分单位）
    PositionLimits pl{100, 1'000'000};
    const InstrumentId inst = 42;
};

TEST_F(PositionLimitsTest, NormalBuySucceeds) {
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::BUY, 10, 1000));
    EXPECT_EQ(pl.get_net_qty(inst), 10);
}

TEST_F(PositionLimitsTest, NormalSellSucceeds) {
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::SELL, 10, 1000));
    EXPECT_EQ(pl.get_net_qty(inst), -10);
}

TEST_F(PositionLimitsTest, ExceedMaxNetQtyRejected) {
    // 先买 90
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::BUY, 90, 100));
    // 再买 11 超限（90+11=101 > 100）
    EXPECT_FALSE(pl.check_and_reserve(inst, Side::BUY, 11, 100));
    // 持仓未变
    EXPECT_EQ(pl.get_net_qty(inst), 90);
}

TEST_F(PositionLimitsTest, ExactMaxNetQtySucceeds) {
    // 恰好买满 100
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::BUY, 100, 100));
    EXPECT_EQ(pl.get_net_qty(inst), 100);
}

TEST_F(PositionLimitsTest, ExceedMaxNotionalRejected) {
    // price=10001, qty=100 → notional=1,000,100 > 1,000,000
    EXPECT_FALSE(pl.check_and_reserve(inst, Side::BUY, 100, 10001));
    EXPECT_EQ(pl.get_net_qty(inst), 0);
}

TEST_F(PositionLimitsTest, ExactMaxNotionalSucceeds) {
    // price=10000, qty=100 → notional=1,000,000 = max
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::BUY, 100, 10000));
}

TEST_F(PositionLimitsTest, CancelRestoresQuota) {
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::BUY, 90, 100));
    EXPECT_EQ(pl.get_net_qty(inst), 90);

    // 撤单归还 90 手
    pl.on_cancel(inst, Side::BUY, 90);
    EXPECT_EQ(pl.get_net_qty(inst), 0);

    // 归还后可以再买 100 手
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::BUY, 100, 100));
}

TEST_F(PositionLimitsTest, NetPositionBuySellBalance) {
    (void)pl.check_and_reserve(inst, Side::BUY,  50, 100);
    (void)pl.check_and_reserve(inst, Side::SELL, 30, 100);
    EXPECT_EQ(pl.get_net_qty(inst), 20);
}

TEST_F(PositionLimitsTest, InvalidInstrumentIdRejected) {
    EXPECT_FALSE(pl.check_and_reserve(MAX_INSTRUMENTS, Side::BUY, 1, 100));
    EXPECT_FALSE(pl.check_and_reserve(MAX_INSTRUMENTS + 1, Side::BUY, 1, 100));
}

TEST_F(PositionLimitsTest, ShortPositionLimit) {
    // 卖出超过 -100 应被拒绝
    EXPECT_TRUE(pl.check_and_reserve(inst, Side::SELL, 100, 100));
    EXPECT_EQ(pl.get_net_qty(inst), -100);
    EXPECT_FALSE(pl.check_and_reserve(inst, Side::SELL, 1, 100));
}

// ============================================================
// PreTradeRisk 测试
// ============================================================
class PreTradeRiskTest : public ::testing::Test {
protected:
    CircuitBreaker cb{-5000};
    RateLimiter    rl{1000, 100};   // 高速率，测试中不成为瓶颈
    PositionLimits pl{100, 1'000'000};
    PreTradeRisk   risk{cb, rl, pl};

    // 构造一个合法的 OrderRequest
    OrderRequest make_req(InstrumentId id = 1,
                          Side s = Side::BUY,
                          Qty q = 10,
                          Price p = 1000) {
        OrderRequest req;
        req.instrument_id = id;
        req.side          = s;
        req.qty           = q;
        req.price         = p;
        req.type          = OrderType::LIMIT;
        return req;
    }
};

TEST_F(PreTradeRiskTest, ValidOrderPasses) {
    EXPECT_TRUE(risk.check(make_req()));
}

TEST_F(PreTradeRiskTest, CircuitBreakerBlocksOrder) {
    cb.update_pnl(-9999);   // 触发熔断
    ASSERT_EQ(cb.state(), CBState::OPEN);
    EXPECT_FALSE(risk.check(make_req()));
}

TEST_F(PreTradeRiskTest, RateLimiterBlocksOrder) {
    // burst=100，先消耗完
    RateLimiter tight_rl{1, 1};
    PreTradeRisk tight_risk{cb, tight_rl, pl};

    EXPECT_TRUE(tight_risk.check(make_req()));   // 第 1 次成功（burst=1）
    EXPECT_FALSE(tight_risk.check(make_req()));  // 第 2 次被速率限制
}

TEST_F(PreTradeRiskTest, PositionLimitsBlocksOrder) {
    // 先买满持仓上限
    auto fill_req = make_req(1, Side::BUY, 100, 1000);
    EXPECT_TRUE(risk.check(fill_req));
    // 再买 1 手，持仓超限
    EXPECT_FALSE(risk.check(make_req(1, Side::BUY, 1, 1000)));
}

TEST_F(PreTradeRiskTest, NotionalLimitBlocksOrder) {
    // price × qty = 10001 × 100 = 1,000,100 > 1,000,000
    auto req = make_req(1, Side::BUY, 100, 10001);
    EXPECT_FALSE(risk.check(req));
}

TEST_F(PreTradeRiskTest, AllChecksPassOnMultipleInstruments) {
    // 不同品种互不影响
    EXPECT_TRUE(risk.check(make_req(1, Side::BUY,  10, 1000)));
    EXPECT_TRUE(risk.check(make_req(2, Side::SELL, 10, 1000)));
    EXPECT_TRUE(risk.check(make_req(3, Side::BUY,  50, 500)));
}

TEST_F(PreTradeRiskTest, ResetCircuitBreakerAllowsOrders) {
    cb.update_pnl(-9999);
    ASSERT_EQ(cb.state(), CBState::OPEN);
    EXPECT_FALSE(risk.check(make_req()));

    cb.reset();
    EXPECT_TRUE(risk.check(make_req()));
}

// ============================================================
// main
// ============================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
