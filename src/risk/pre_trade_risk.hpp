#pragma once
#include "circuit_breaker.hpp"
#include "rate_limiter.hpp"
#include "position_limits.hpp"
#include "oms/order.hpp"

namespace hft {

// Pre-trade 风控汇总入口
// 按优先级短路执行：熔断 → 速率 → 持仓/名义金额
class PreTradeRisk {
public:
    PreTradeRisk(
        CircuitBreaker&  cb,
        RateLimiter&     rl,
        PositionLimits&  pl) noexcept;

    // 执行所有 Pre-trade 检查，任一失败立即返回 false（短路求值）
    // 检查顺序：
    //   1. circuit_breaker_.allow_order()
    //   2. rate_limiter_.try_acquire()
    //   3. position_limits_.check_and_reserve(...)
    [[nodiscard]] bool check(const OrderRequest& req) noexcept;

private:
    CircuitBreaker& circuit_breaker_;
    RateLimiter&    rate_limiter_;
    PositionLimits& position_limits_;
};

} // namespace hft
