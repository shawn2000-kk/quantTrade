#pragma once
#include <atomic>
#include <cstdint>
#include "common/types.hpp"

namespace hft {

// 熔断器状态机
// CLOSED（正常） → OPEN（熔断中）← update_pnl 触发
// OPEN → CLOSED（reset 手动恢复，或运营人员调用）
class CircuitBreaker {
public:
    // open_threshold: 触发熔断的亏损阈值（必须为负值，单位：分）
    explicit CircuitBreaker(int64_t open_threshold) noexcept;

    // 是否允许报单：CLOSED 返回 true，其余返回 false
    // memory_order_relaxed：热路径，只要最终可见即可
    [[nodiscard]] bool allow_order() const noexcept;

    // 原子累加 PnL 增量，若累计 PnL 低于 open_threshold_ 则触发熔断（CLOSED→OPEN）
    void update_pnl(int64_t delta) noexcept;

    // 手动重置到 CLOSED（运营人员在日终/风控审查后调用）
    void reset() noexcept;

    // 查询当前状态
    [[nodiscard]] CBState state() const noexcept;

    // 查询当前累计 PnL（单位：分）
    [[nodiscard]] int64_t daily_pnl() const noexcept;

private:
    alignas(64) std::atomic<CBState>  state_;
    alignas(64) std::atomic<int64_t>  daily_pnl_;
    int64_t open_threshold_;       // 触发熔断阈值（负值）
    int64_t half_open_threshold_;  // 允许探测恢复的阈值 = open_threshold_ * 0.5
};

} // namespace hft
