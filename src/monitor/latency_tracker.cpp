// ============================================================
// latency_tracker.cpp
// ============================================================

#include "monitor/latency_tracker.hpp"
#include "infra/rdtsc_clock.hpp"

#include <algorithm>
#include <numeric>
#include <cstring>

namespace hft {

// ------------------------------------------------------------
// LatencyTracker
// ------------------------------------------------------------

LatencyTracker::LatencyTracker(std::string_view name,
                               size_t window_size) noexcept
    : name_(name)
    , window_size_(window_size > 0 ? window_size : 1)
{
    samples_.resize(window_size_);
    // 预填 0，防止未初始化内存参与统计
    std::fill(samples_.begin(), samples_.end(), uint64_t{0});
}

void LatencyTracker::record(uint64_t latency_ns) noexcept {
    // fetch_add 返回写前的值；写入位置为 old_count % window_size_
    const uint64_t idx = count_.fetch_add(1, std::memory_order_relaxed)
                         % static_cast<uint64_t>(window_size_);
    samples_[idx] = latency_ns;
}

uint64_t LatencyTracker::percentile(double p) const noexcept {
    // 有效样本数：min(count_, window_size_)
    const uint64_t total = count_.load(std::memory_order_relaxed);
    const size_t   n     = static_cast<size_t>(
        total < static_cast<uint64_t>(window_size_) ? total : window_size_);
    if (n == 0) return 0;

    // 拷贝 snapshot 并排序（monitoring 路径，允许堆分配）
    std::vector<uint64_t> tmp(samples_.begin(), samples_.begin() + static_cast<ptrdiff_t>(n));
    std::sort(tmp.begin(), tmp.end());

    // p 钳位至 [0.0, 1.0]
    const double clamped = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
    // 线性插值索引（nearest-rank 方法）
    const size_t idx = static_cast<size_t>(clamped * static_cast<double>(n - 1) + 0.5);
    return tmp[idx < n ? idx : n - 1];
}

uint64_t LatencyTracker::min_ns() const noexcept {
    const uint64_t total = count_.load(std::memory_order_relaxed);
    const size_t   n     = static_cast<size_t>(
        total < static_cast<uint64_t>(window_size_) ? total : window_size_);
    if (n == 0) return 0;
    return *std::min_element(samples_.begin(), samples_.begin() + static_cast<ptrdiff_t>(n));
}

uint64_t LatencyTracker::max_ns() const noexcept {
    const uint64_t total = count_.load(std::memory_order_relaxed);
    const size_t   n     = static_cast<size_t>(
        total < static_cast<uint64_t>(window_size_) ? total : window_size_);
    if (n == 0) return 0;
    return *std::max_element(samples_.begin(), samples_.begin() + static_cast<ptrdiff_t>(n));
}

double LatencyTracker::mean_ns() const noexcept {
    const uint64_t total = count_.load(std::memory_order_relaxed);
    const size_t   n     = static_cast<size_t>(
        total < static_cast<uint64_t>(window_size_) ? total : window_size_);
    if (n == 0) return 0.0;
    const uint64_t sum = std::accumulate(
        samples_.begin(),
        samples_.begin() + static_cast<ptrdiff_t>(n),
        uint64_t{0});
    return static_cast<double>(sum) / static_cast<double>(n);
}

uint64_t LatencyTracker::count() const noexcept {
    return count_.load(std::memory_order_relaxed);
}

void LatencyTracker::reset() noexcept {
    count_.store(0, std::memory_order_relaxed);
    std::fill(samples_.begin(), samples_.end(), uint64_t{0});
}

// ------------------------------------------------------------
// ScopedLatency
// ------------------------------------------------------------

ScopedLatency::ScopedLatency(LatencyTracker& tracker) noexcept
    : t0_(rdtsc_ns())
    , tracker_(tracker)
{}

ScopedLatency::~ScopedLatency() noexcept {
    const uint64_t t1 = rdtsc_ns();
    // 防止时钟倒退（不同核心 TSC 未同步时极罕见）
    if (t1 > t0_) {
        tracker_.record(t1 - t0_);
    } else {
        tracker_.record(0);
    }
}

} // namespace hft
