#pragma once

// ============================================================
// latency_tracker.hpp — TSC 延迟统计（循环缓冲 + 百分位）
//
// - LatencyTracker：固定大小循环缓冲，无动态扩容，原子 count_
// - ScopedLatency：RAII 辅助，析构时自动 record
// ============================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace hft {

// ------------------------------------------------------------
// LatencyTracker
// ------------------------------------------------------------
class LatencyTracker {
public:
    /// @param name        标识符（日志/Prometheus label 用）
    /// @param window_size 循环缓冲容量（保留最近 N 个样本）
    explicit LatencyTracker(std::string_view name,
                            size_t window_size = 1'000'000) noexcept;

    // 不可拷贝/移动（内部持有 std::atomic，不支持移动语义）
    LatencyTracker(const LatencyTracker&)            = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;
    LatencyTracker(LatencyTracker&&)                 = delete;
    LatencyTracker& operator=(LatencyTracker&&)      = delete;

    // --------------------------------------------------------
    // 写入路径（热路径，noexcept）
    // --------------------------------------------------------

    /// 写入一个延迟样本（纳秒）。
    /// 若 window 已满则覆盖最老的条目（循环缓冲语义）。
    void record(uint64_t latency_ns) noexcept;

    // --------------------------------------------------------
    // 查询路径（监控线程调用，允许少量开销）
    // --------------------------------------------------------

    /// 返回第 p 百分位（0.0 ~ 1.0）。
    /// 实现：对当前有效样本拷贝后排序，O(N log N)。
    /// @return 0 若无样本
    [[nodiscard]] uint64_t percentile(double p) const noexcept;

    [[nodiscard]] uint64_t min_ns()  const noexcept;
    [[nodiscard]] uint64_t max_ns()  const noexcept;
    [[nodiscard]] double   mean_ns() const noexcept;

    /// 已写入的总样本数（包含已覆盖的历史样本）
    [[nodiscard]] uint64_t count() const noexcept;

    /// 清零 count_ 并将所有 samples_ 置为 0
    void reset() noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    std::string            name_;
    size_t                 window_size_;
    std::vector<uint64_t>  samples_;          // 固定大小，预分配于构造时
    alignas(64) std::atomic<uint64_t> count_{0};  // 写指针：count_ % window_size_
};

// ------------------------------------------------------------
// ScopedLatency — RAII 延迟打点
//
//   {
//       ScopedLatency sl(tracker);      // 构造：记录 t0
//       do_work();
//   }                                   // 析构：record(now - t0)
// ------------------------------------------------------------
class ScopedLatency {
public:
    explicit ScopedLatency(LatencyTracker& tracker) noexcept;
    ~ScopedLatency() noexcept;

    // 不可拷贝/移动（防止意外多次 record）
    ScopedLatency(const ScopedLatency&)            = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;
    ScopedLatency(ScopedLatency&&)                 = delete;
    ScopedLatency& operator=(ScopedLatency&&)      = delete;

private:
    uint64_t        t0_;
    LatencyTracker& tracker_;
};

} // namespace hft
