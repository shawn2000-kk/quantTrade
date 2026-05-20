#include "risk/pre_trade_risk.hpp"

namespace hft {

PreTradeRisk::PreTradeRisk(
    CircuitBreaker& cb,
    RateLimiter&    rl,
    PositionLimits& pl) noexcept
    : circuit_breaker_{cb}
    , rate_limiter_{rl}
    , position_limits_{pl}
{
}

bool PreTradeRisk::check(const OrderRequest& req) noexcept {
    // 短路求值：任一失败立即返回 false，不执行后续检查
    // 顺序：熔断（最快，单原子读）→ 速率（CAS）→ 持仓（CAS）

    if (!circuit_breaker_.allow_order()) return false;
    if (!rate_limiter_.try_acquire())    return false;
    if (!position_limits_.check_and_reserve(
            req.instrument_id, req.side, req.qty, req.price)) return false;

    return true;
}

} // namespace hft
