#pragma once

#include <cstdint>
#include <chrono>
#include "common/types.hpp"

// ============================================================
// rdtsc_clock.hpp — TSC 高精度时钟（纳秒级）
//
// Linux:  使用 rdtscp 指令（串行化），乘以 ns_per_cycle 返回纳秒。
// macOS:  rdtscp 行为不受保证，fallback 到 clock_gettime(CLOCK_MONOTONIC_RAW)。
// ============================================================

namespace hft {

// ------------------------------------------------------------
// 裸 TSC / 平台 fallback
// ------------------------------------------------------------
#ifdef __linux__

/// 读取 TSC（rdtscp：含 serialize，防止乱序执行）。
/// 返回裸 TSC 计数，不做单位换算。
[[nodiscard]] inline uint64_t raw_tsc() noexcept {
    uint32_t lo, hi;
    __asm__ volatile(
        "rdtscp"
        : "=a"(lo), "=d"(hi)
        :: "ecx"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

#else // macOS / other POSIX

#include <time.h>

[[nodiscard]] inline uint64_t raw_tsc() noexcept {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
           + static_cast<uint64_t>(ts.tv_nsec);
}

#endif // __linux__

// ------------------------------------------------------------
// RdtscClock 单例
// ------------------------------------------------------------

class RdtscClock {
public:
    /// 单例访问（Meyer's singleton，线程安全）
    static RdtscClock& instance() noexcept {
        static RdtscClock inst;
        return inst;
    }

    // 禁止拷贝 / 移动
    RdtscClock(const RdtscClock&)            = delete;
    RdtscClock& operator=(const RdtscClock&) = delete;

    // --------------------------------------------------------
    // calibrate() — 计算 ns_per_cycle_
    //
    // 采样策略：在 sleep_us 微秒的时间窗口内，同时记录
    // steady_clock 和 TSC 的 delta，多次采样取中位数。
    // --------------------------------------------------------
    void calibrate(int samples = 5, int sleep_us = 200) noexcept {
#ifdef __linux__
        // 在 Linux TSC 路径下进行真实校准
        double best = 0.0;
        double sum  = 0.0;
        int    cnt  = 0;

        for (int i = 0; i < samples; ++i) {
            using Clock = std::chrono::steady_clock;

            const uint64_t tsc0  = raw_tsc();
            const auto     wall0 = Clock::now();

            // 短暂忙等（避免 sleep 系统调用误差）
            volatile uint64_t spin = 0;
            auto deadline = wall0 + std::chrono::microseconds(sleep_us);
            while (Clock::now() < deadline) { ++spin; }

            const uint64_t tsc1  = raw_tsc();
            const auto     wall1 = Clock::now();

            const double dt_ns  = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count()
            );
            const double dt_tsc = static_cast<double>(tsc1 - tsc0);

            if (dt_tsc > 0.0 && dt_ns > 0.0) {
                const double ratio = dt_ns / dt_tsc;
                sum += ratio;
                ++cnt;
                (void)best;
            }
        }

        if (cnt > 0) {
            ns_per_cycle_ = sum / static_cast<double>(cnt);
        }
        // 保留校准时刻的偏移，使 now_ns() 近似对齐 CLOCK_MONOTONIC_RAW
        base_tsc_  = raw_tsc();
        base_ns_   = [&]() -> uint64_t {
            using Clock = std::chrono::steady_clock;
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch()
                ).count()
            );
        }();
#else
        // macOS: raw_tsc() 已经是纳秒，无需额外校准
        (void)samples;
        (void)sleep_us;
        ns_per_cycle_ = 1.0;
        base_tsc_     = raw_tsc();
        base_ns_      = base_tsc_;
#endif
        calibrated_ = true;
    }

    // --------------------------------------------------------
    // now_ns() — 返回当前时间戳（纳秒）
    // --------------------------------------------------------
    [[nodiscard]] uint64_t now_ns() const noexcept {
#ifdef __linux__
        const uint64_t delta_tsc = raw_tsc() - base_tsc_;
        return base_ns_ + static_cast<uint64_t>(
            static_cast<double>(delta_tsc) * ns_per_cycle_
        );
#else
        return raw_tsc(); // 已经是纳秒
#endif
    }

    /// 裸 TSC 值，用于差值计算（不做单位换算）。
    [[nodiscard]] static uint64_t tsc() noexcept {
        return raw_tsc();
    }

    /// ns_per_cycle 访问器（调试 / 日志用）
    [[nodiscard]] double ns_per_cycle() const noexcept { return ns_per_cycle_; }

    [[nodiscard]] bool is_calibrated() const noexcept { return calibrated_; }

private:
    RdtscClock() noexcept {
        calibrate(); // 构造时自动校准一次
    }

    double   ns_per_cycle_{1.0};
    uint64_t base_tsc_{0};
    uint64_t base_ns_{0};
    bool     calibrated_{false};
};

// ------------------------------------------------------------
// 便捷全局函数
// ------------------------------------------------------------

/// 返回当前纳秒时间戳（等价于 RdtscClock::instance().now_ns()）
[[nodiscard]] inline uint64_t rdtsc_ns() noexcept {
    return RdtscClock::instance().now_ns();
}

} // namespace hft
