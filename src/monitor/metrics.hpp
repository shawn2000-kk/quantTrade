#pragma once

// ============================================================
// metrics.hpp — Prometheus 风格指标（纯手写，不依赖 prometheus-cpp）
//
// Counter     单调递增计数器
// Gauge       可增可减的瞬时值
// Histogram   固定 bucket 边界的直方图，输出 Prometheus text format
// MetricsRegistry 单例注册表，按名称管理所有指标
// ============================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hft {

// ------------------------------------------------------------
// Counter — 单调递增计数器
// ------------------------------------------------------------
class Counter {
public:
    Counter() noexcept = default;

    // 禁止拷贝（std::atomic 不可拷贝）
    Counter(const Counter&)            = delete;
    Counter& operator=(const Counter&) = delete;

    /// 递增 n（默认 1）
    void inc(int64_t n = 1) noexcept {
        value_.fetch_add(n, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<int64_t> value_{0};
};

// ------------------------------------------------------------
// Gauge — 可增可减的瞬时值
// ------------------------------------------------------------
class Gauge {
public:
    Gauge() noexcept = default;

    Gauge(const Gauge&)            = delete;
    Gauge& operator=(const Gauge&) = delete;

    void set(int64_t v) noexcept {
        value_.store(v, std::memory_order_relaxed);
    }
    void inc(int64_t n = 1) noexcept {
        value_.fetch_add(n, std::memory_order_relaxed);
    }
    void dec(int64_t n = 1) noexcept {
        value_.fetch_sub(n, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<int64_t> value_{0};
};

// ------------------------------------------------------------
// Histogram — 固定 bucket 直方图
//
// 边界语义：le（less-or-equal），与 Prometheus 一致。
// 样本 v 落入所有满足 boundaries_[i] >= v 的 bucket。
//
// 输出格式（Prometheus text format）：
//   name_bucket{le="100"} 5
//   name_bucket{le="500"} 8
//   name_bucket{le="+Inf"} 12
//   name_sum 45000
//   name_count 12
// ------------------------------------------------------------
class Histogram {
public:
    /// @param boundaries 升序排列的上界列表（单位同 observe() 的参数）
    explicit Histogram(std::vector<uint64_t> boundaries) noexcept;

    Histogram(const Histogram&)            = delete;
    Histogram& operator=(const Histogram&) = delete;

    /// 记录一个观测值
    void observe(uint64_t v) noexcept;

    /// 输出 Prometheus text format 片段
    /// @param name 指标名称（不含 _bucket/_sum/_count 后缀）
    [[nodiscard]] std::string to_prometheus_text(std::string_view name) const;

    [[nodiscard]] int64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t sum() const noexcept {
        return sum_.load(std::memory_order_relaxed);
    }

private:
    std::vector<uint64_t>                      boundaries_;
    // unique_ptr<T[]> 规避 std::atomic 不可移动的 vector 限制
    std::unique_ptr<std::atomic<int64_t>[]>    buckets_;
    size_t                                     num_buckets_{0};
    alignas(64) std::atomic<int64_t>           count_{0};
    alignas(64) std::atomic<uint64_t>          sum_{0};
};

// ------------------------------------------------------------
// MetricsRegistry — 单例注册表
//
// 按名称懒惰创建并缓存 Counter / Gauge。
// 所有方法线程安全（内部持有 std::mutex）。
// ------------------------------------------------------------
class MetricsRegistry {
public:
    /// Meyer's singleton（线程安全）
    static MetricsRegistry& instance() noexcept;

    // 禁止拷贝/移动
    MetricsRegistry(const MetricsRegistry&)            = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    /// 按名称获取（或创建）Counter
    Counter& counter(std::string_view name);

    /// 按名称获取（或创建）Gauge
    Gauge& gauge(std::string_view name);

    /// 输出所有 Counter 和 Gauge 的 Prometheus text（按名称排序）
    [[nodiscard]] std::string dump_all() const;

private:
    MetricsRegistry() noexcept = default;

    mutable std::mutex                                   mu_;
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>>   gauges_;
};

} // namespace hft
