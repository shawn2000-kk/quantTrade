// ============================================================
// metrics.cpp
// ============================================================

#include "monitor/metrics.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <vector>

namespace hft {

// ------------------------------------------------------------
// 内部辅助：将 uint64_t 追加到 std::string（避免 to_string 的 locale）
// ------------------------------------------------------------
namespace {

void append_u64(std::string& out, uint64_t v) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, ptr);
}

void append_i64(std::string& out, int64_t v) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, ptr);
}

} // namespace

// ------------------------------------------------------------
// Histogram
// ------------------------------------------------------------

Histogram::Histogram(std::vector<uint64_t> boundaries) noexcept
    : boundaries_(std::move(boundaries))
    , num_buckets_(boundaries_.size())
{
    // 确保边界升序
    std::sort(boundaries_.begin(), boundaries_.end());
    // make_unique<T[]>(n) 对每个元素调用默认构造（atomic 默认初始化为 0）
    buckets_ = std::make_unique<std::atomic<int64_t>[]>(num_buckets_);
}

void Histogram::observe(uint64_t v) noexcept {
    // 对每个 le 边界，若 v <= boundary，递增该 bucket
    for (size_t i = 0; i < num_buckets_; ++i) {
        if (v <= boundaries_[i]) {
            buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    sum_.fetch_add(v, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
}

std::string Histogram::to_prometheus_text(std::string_view name) const {
    std::string out;
    out.reserve(256);

    const int64_t  total  = count_.load(std::memory_order_relaxed);
    const uint64_t s      = sum_.load(std::memory_order_relaxed);

    // _bucket{le="..."} N
    for (size_t i = 0; i < num_buckets_; ++i) {
        out += name;
        out += "_bucket{le=\"";
        append_u64(out, boundaries_[i]);
        out += "\"} ";
        append_i64(out, buckets_[i].load(std::memory_order_relaxed));
        out += '\n';
    }

    // +Inf bucket（= total count）
    out += name;
    out += "_bucket{le=\"+Inf\"} ";
    append_i64(out, total);
    out += '\n';

    // _sum
    out += name;
    out += "_sum ";
    append_u64(out, s);
    out += '\n';

    // _count
    out += name;
    out += "_count ";
    append_i64(out, total);
    out += '\n';

    return out;
}

// ------------------------------------------------------------
// MetricsRegistry
// ------------------------------------------------------------

MetricsRegistry& MetricsRegistry::instance() noexcept {
    static MetricsRegistry inst;
    return inst;
}

Counter& MetricsRegistry::counter(std::string_view name) {
    const std::string key(name);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(key);
    if (it == counters_.end()) {
        auto [inserted, ok] = counters_.emplace(key, std::make_unique<Counter>());
        (void)ok;
        return *inserted->second;
    }
    return *it->second;
}

Gauge& MetricsRegistry::gauge(std::string_view name) {
    const std::string key(name);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = gauges_.find(key);
    if (it == gauges_.end()) {
        auto [inserted, ok] = gauges_.emplace(key, std::make_unique<Gauge>());
        (void)ok;
        return *inserted->second;
    }
    return *it->second;
}

std::string MetricsRegistry::dump_all() const {
    std::string out;
    out.reserve(4096);

    // 收集并排序输出，保证确定性
    std::vector<std::pair<std::string, int64_t>> items;

    {
        std::lock_guard<std::mutex> lk(mu_);

        items.reserve(counters_.size() + gauges_.size());

        for (const auto& [k, v] : counters_) {
            items.emplace_back(k, v->get());
        }
        for (const auto& [k, v] : gauges_) {
            items.emplace_back(k, v->get());
        }
    }

    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    for (const auto& [k, v] : items) {
        out += k;
        out += ' ';
        char buf[24];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, ptr);
        out += '\n';
    }

    return out;
}

} // namespace hft
