#include "risk/rate_limiter.hpp"
#include <time.h>

namespace hft {

// --------------------------------------------------------------------------
// 内部时间获取：CLOCK_MONOTONIC_RAW，纳秒精度
// 避免 TSC 频率校准复杂性，保证跨核心一致
// --------------------------------------------------------------------------
int64_t RateLimiter::get_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// --------------------------------------------------------------------------
// 构造
// tokens_     内部以 "令牌数 × 1000" 存储，避免浮点
// burst_scaled = burst × 1000
// rate_ns_scaled = rate_per_sec × 1000  （每纳秒产生 rate_per_sec/1e9 个令牌
//                                         → 每纳秒产生 rate_per_sec×1000/1e9 个 scaled 令牌
//                                         → 实际在 refill 里用 elapsed_ns * rate_per_sec / 1e6 计算）
// --------------------------------------------------------------------------
RateLimiter::RateLimiter(int64_t rate_per_sec, int64_t burst) noexcept
    : tokens_{burst * 1000}
    , last_refill_ns_{get_ns()}
    , burst_scaled_{burst * 1000}
    , rate_ns_scaled_{rate_per_sec * 1000}  // scaled 令牌 / 秒
{
}

// --------------------------------------------------------------------------
// 令牌补充
// elapsed_ns 纳秒内新增令牌（scaled）= elapsed_ns * rate_per_sec_ / 1e9 * 1000
//                                     = elapsed_ns * rate_ns_scaled_ / 1e9
// 使用整数除法，误差在 1/1000 个令牌以内（亚毫秒级，可接受）
// --------------------------------------------------------------------------
int64_t RateLimiter::refill(int64_t now_ns) noexcept {
    int64_t last = last_refill_ns_.load(std::memory_order_relaxed);
    int64_t elapsed_ns = now_ns - last;
    if (elapsed_ns <= 0) {
        return tokens_.load(std::memory_order_relaxed);
    }

    // 新增令牌（scaled）
    int64_t add_scaled = elapsed_ns * rate_ns_scaled_ / 1'000'000'000LL;
    if (add_scaled <= 0) {
        // 时间太短，令牌增量不足 1/1000，直接返回当前值
        return tokens_.load(std::memory_order_relaxed);
    }

    // 更新 last_refill_ns_（CAS：允许并发调用，只有一个线程成功更新）
    // 失败也没关系，下次调用会再次计算
    last_refill_ns_.compare_exchange_strong(
        last, now_ns,
        std::memory_order_relaxed,
        std::memory_order_relaxed);

    // 原子加，但不超过 burst
    int64_t old_tokens = tokens_.load(std::memory_order_relaxed);
    int64_t new_tokens;
    do {
        new_tokens = old_tokens + add_scaled;
        if (new_tokens > burst_scaled_) new_tokens = burst_scaled_;
    } while (!tokens_.compare_exchange_weak(
        old_tokens, new_tokens,
        std::memory_order_relaxed,
        std::memory_order_relaxed));

    return new_tokens;
}

// --------------------------------------------------------------------------
// try_acquire
// 1. 先补充令牌
// 2. CAS 消耗 n 个令牌（scaled: n × 1000）
// --------------------------------------------------------------------------
bool RateLimiter::try_acquire(int64_t n) noexcept {
    int64_t now = get_ns();
    refill(now);

    int64_t need = n * 1000;
    int64_t cur = tokens_.load(std::memory_order_relaxed);
    do {
        if (cur < need) return false;
    } while (!tokens_.compare_exchange_weak(
        cur, cur - need,
        std::memory_order_relaxed,
        std::memory_order_relaxed));

    return true;
}

} // namespace hft
