#include "risk/circuit_breaker.hpp"

namespace hft {

CircuitBreaker::CircuitBreaker(int64_t open_threshold) noexcept
    : state_{CBState::CLOSED}
    , daily_pnl_{0}
    , open_threshold_{open_threshold}
    , half_open_threshold_{open_threshold / 2}  // 50% 阈值，用于 HALF_OPEN 探测（当前实现中预留接口）
{
}

bool CircuitBreaker::allow_order() const noexcept {
    return state_.load(std::memory_order_relaxed) == CBState::CLOSED;
}

void CircuitBreaker::update_pnl(int64_t delta) noexcept {
    // 原子累加 PnL
    const int64_t new_pnl = daily_pnl_.fetch_add(delta, std::memory_order_relaxed) + delta;

    // PnL 跌破 open_threshold_ → CLOSED 转 OPEN（CAS 保证只触发一次）
    if (new_pnl <= open_threshold_) {
        CBState expected = CBState::CLOSED;
        state_.compare_exchange_strong(
            expected,
            CBState::OPEN,
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }
    // PnL 回升至 half_open_threshold_ 以上 → OPEN 转 HALF_OPEN（允许人工探测恢复）
    // half_open_threshold_ = open_threshold_ × 0.5（绝对值更小，损失已部分回收）
    else if (new_pnl > half_open_threshold_) {
        CBState expected = CBState::OPEN;
        state_.compare_exchange_strong(
            expected,
            CBState::HALF_OPEN,
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }
}

void CircuitBreaker::reset() noexcept {
    // 运营人员手动恢复：重置 PnL 累计并将状态切回 CLOSED
    daily_pnl_.store(0, std::memory_order_relaxed);
    state_.store(CBState::CLOSED, std::memory_order_release);
}

CBState CircuitBreaker::state() const noexcept {
    return state_.load(std::memory_order_relaxed);
}

int64_t CircuitBreaker::daily_pnl() const noexcept {
    return daily_pnl_.load(std::memory_order_relaxed);
}

} // namespace hft
