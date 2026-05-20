#pragma once
#include <atomic>
#include <cstdint>

namespace hft {

// Token Bucket 报单速率限制
// tokens_ 内部以 × 1000 整数存储，避免浮点运算
// 线程安全：全原子操作，无锁
class RateLimiter {
public:
    // rate_per_sec: 每秒最大令牌数（稳态）
    // burst:        桶容量上限（允许短时突发）
    RateLimiter(int64_t rate_per_sec, int64_t burst) noexcept;

    // 尝试消耗 n 个令牌
    // 先补充自上次调用以来积累的令牌，再尝试消耗
    // 成功返回 true，令牌不足返回 false
    [[nodiscard]] bool try_acquire(int64_t n = 1) noexcept;

private:
    // 内部：获取单调时间（纳秒）
    static int64_t get_ns() noexcept;

    // 内部：按时间差补充令牌，返回补充后的令牌数（已 ×1000）
    int64_t refill(int64_t now_ns) noexcept;

    alignas(64) std::atomic<int64_t> tokens_;         // 当前令牌数 × 1000
    alignas(64) std::atomic<int64_t> last_refill_ns_; // 上次补充时间（纳秒）

    int64_t burst_scaled_;    // 桶容量上限 × 1000
    int64_t rate_ns_scaled_;  // 每纳秒产生令牌数 × 1000 × 1e9（用整数除法避免浮点）
};

} // namespace hft
