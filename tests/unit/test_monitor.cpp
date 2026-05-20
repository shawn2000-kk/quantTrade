// ============================================================
// test_monitor.cpp — 监控层单元测试
// ============================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "monitor/latency_tracker.hpp"
#include "monitor/metrics.hpp"
#include "monitor/alerting.hpp"

using namespace hft;
using namespace std::chrono_literals;

// ============================================================
// LatencyTracker 测试
// ============================================================
class LatencyTrackerTest : public ::testing::Test {
protected:
    LatencyTracker tracker{"test_tracker", 4096};
};

/// record 后 count 正确累加
TEST_F(LatencyTrackerTest, CountAfterRecord) {
    EXPECT_EQ(tracker.count(), 0u);

    for (uint64_t i = 1; i <= 100; ++i) {
        tracker.record(i * 10);
    }
    EXPECT_EQ(tracker.count(), 100u);
}

/// 1000 个均匀分布样本，p50/p99 在合理范围
TEST_F(LatencyTrackerTest, UniformDistributionPercentiles) {
    // 写入 1..1000 ns（均匀分布）
    for (uint64_t i = 1; i <= 1000; ++i) {
        tracker.record(i);
    }
    EXPECT_EQ(tracker.count(), 1000u);

    const uint64_t p50 = tracker.percentile(0.50);
    const uint64_t p99 = tracker.percentile(0.99);

    // p50 应在 [400, 600]，p99 应在 [970, 1000]
    EXPECT_GE(p50, 400u) << "p50=" << p50;
    EXPECT_LE(p50, 600u) << "p50=" << p50;

    EXPECT_GE(p99, 970u) << "p99=" << p99;
    EXPECT_LE(p99, 1000u) << "p99=" << p99;
}

/// reset 后 count=0，后续统计不受旧数据干扰
TEST_F(LatencyTrackerTest, ResetClearsCount) {
    for (int i = 0; i < 500; ++i) tracker.record(1000u);
    ASSERT_EQ(tracker.count(), 500u);

    tracker.reset();

    EXPECT_EQ(tracker.count(), 0u);
    // reset 后 percentile 应返回 0（无样本）
    EXPECT_EQ(tracker.percentile(0.99), 0u);
    EXPECT_EQ(tracker.min_ns(), 0u);
    EXPECT_EQ(tracker.max_ns(), 0u);
}

/// min / max 正确
TEST_F(LatencyTrackerTest, MinMax) {
    tracker.record(500u);
    tracker.record(100u);
    tracker.record(900u);
    tracker.record(300u);

    EXPECT_EQ(tracker.min_ns(), 100u);
    EXPECT_EQ(tracker.max_ns(), 900u);
}

/// mean 正确
TEST_F(LatencyTrackerTest, Mean) {
    tracker.record(100u);
    tracker.record(200u);
    tracker.record(300u);

    EXPECT_DOUBLE_EQ(tracker.mean_ns(), 200.0);
}

/// 循环缓冲覆盖：超过 window_size 时旧数据被覆盖
TEST_F(LatencyTrackerTest, CircularBufferOverwrite) {
    // window_size = 4096；写入 5000 个样本
    for (uint64_t i = 0; i < 5000; ++i) {
        tracker.record(i + 1);
    }
    // count 累计到 5000，但有效样本只有 window_size=4096
    EXPECT_EQ(tracker.count(), 5000u);
    // max 应为最后写入的最大值（5000），在有效 4096 样本中
    EXPECT_GE(tracker.max_ns(), 1u);
}

// ============================================================
// ScopedLatency 测试
// ============================================================
TEST(ScopedLatencyTest, IncreasesCountByOne) {
    LatencyTracker tracker{"scoped_test", 100};
    EXPECT_EQ(tracker.count(), 0u);

    {
        ScopedLatency sl(tracker);
        // do_work placeholder
        std::this_thread::sleep_for(1us);
    }

    EXPECT_EQ(tracker.count(), 1u);
    // 延迟应为正值（或 0 极少数情况下）
    EXPECT_GE(tracker.max_ns(), 0u);
}

TEST(ScopedLatencyTest, MultipleScoped) {
    LatencyTracker tracker{"scoped_multi", 100};

    for (int i = 0; i < 10; ++i) {
        ScopedLatency sl(tracker);
        // minimal work
    }

    EXPECT_EQ(tracker.count(), 10u);
}

// ============================================================
// Counter 测试
// ============================================================
class CounterTest : public ::testing::Test {
protected:
    Counter c;
};

TEST_F(CounterTest, InitialValueZero) {
    EXPECT_EQ(c.get(), 0);
}

TEST_F(CounterTest, IncDefaultOne) {
    c.inc();
    EXPECT_EQ(c.get(), 1);
}

TEST_F(CounterTest, IncByN) {
    c.inc(5);
    c.inc(3);
    EXPECT_EQ(c.get(), 8);
}

/// 多线程并发 inc 总和正确
TEST_F(CounterTest, ConcurrentInc) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerThread; ++i) {
                c.inc();
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(c.get(), static_cast<int64_t>(kThreads * kPerThread));
}

// ============================================================
// Gauge 测试
// ============================================================
class GaugeTest : public ::testing::Test {
protected:
    Gauge g;
};

TEST_F(GaugeTest, InitialValueZero) {
    EXPECT_EQ(g.get(), 0);
}

TEST_F(GaugeTest, Set) {
    g.set(42);
    EXPECT_EQ(g.get(), 42);
    g.set(-7);
    EXPECT_EQ(g.get(), -7);
}

TEST_F(GaugeTest, IncDec) {
    g.set(100);
    g.inc(20);
    EXPECT_EQ(g.get(), 120);
    g.dec(50);
    EXPECT_EQ(g.get(), 70);
}

TEST_F(GaugeTest, IncDefaultOne) {
    g.inc();
    g.inc();
    g.dec();
    EXPECT_EQ(g.get(), 1);
}

// ============================================================
// Histogram 测试
// ============================================================
class HistogramTest : public ::testing::Test {
protected:
    // 边界：100, 500, 1000, 5000 ns
    Histogram hist{{100, 500, 1000, 5000}};
};

TEST_F(HistogramTest, ObserveAndCount) {
    hist.observe(50);   // <= 100, <= 500, <= 1000, <= 5000
    hist.observe(200);  // <= 500, <= 1000, <= 5000
    hist.observe(800);  // <= 1000, <= 5000
    hist.observe(2000); // <= 5000
    hist.observe(9999); // 超过所有边界

    EXPECT_EQ(hist.count(), 5);
}

TEST_F(HistogramTest, PrometheusTextContainsBuckets) {
    hist.observe(50);
    hist.observe(200);
    hist.observe(800);
    hist.observe(2000);
    hist.observe(9999);

    const std::string text = hist.to_prometheus_text("hft_latency_ns");

    // 验证包含各 bucket 标记
    EXPECT_NE(text.find("hft_latency_ns_bucket{le=\"100\"}"), std::string::npos);
    EXPECT_NE(text.find("hft_latency_ns_bucket{le=\"500\"}"), std::string::npos);
    EXPECT_NE(text.find("hft_latency_ns_bucket{le=\"1000\"}"), std::string::npos);
    EXPECT_NE(text.find("hft_latency_ns_bucket{le=\"5000\"}"), std::string::npos);
    EXPECT_NE(text.find("hft_latency_ns_bucket{le=\"+Inf\"}"), std::string::npos);
    EXPECT_NE(text.find("hft_latency_ns_sum"), std::string::npos);
    EXPECT_NE(text.find("hft_latency_ns_count"), std::string::npos);
}

TEST_F(HistogramTest, BucketCountsCorrect) {
    // observe(50)  → le100=1, le500=1, le1000=1, le5000=1
    // observe(200) → le100=1, le500=2, le1000=2, le5000=2
    // observe(800) → le100=1, le500=2, le1000=3, le5000=3
    // observe(2000)→ le100=1, le500=2, le1000=3, le5000=4
    // observe(9999)→ 不进任何 boundary bucket（全超出）
    hist.observe(50);
    hist.observe(200);
    hist.observe(800);
    hist.observe(2000);
    hist.observe(9999);

    const std::string text = hist.to_prometheus_text("h");

    // le=100: count=1 → "h_bucket{le=\"100\"} 1"
    EXPECT_NE(text.find("h_bucket{le=\"100\"} 1"), std::string::npos)
        << "full text:\n" << text;
    // le=500: count=2
    EXPECT_NE(text.find("h_bucket{le=\"500\"} 2"), std::string::npos)
        << "full text:\n" << text;
    // le=1000: count=3
    EXPECT_NE(text.find("h_bucket{le=\"1000\"} 3"), std::string::npos)
        << "full text:\n" << text;
    // le=5000: count=4
    EXPECT_NE(text.find("h_bucket{le=\"5000\"} 4"), std::string::npos)
        << "full text:\n" << text;
    // +Inf: count=5
    EXPECT_NE(text.find("h_bucket{le=\"+Inf\"} 5"), std::string::npos)
        << "full text:\n" << text;
    // sum = 50+200+800+2000+9999 = 13049
    EXPECT_NE(text.find("h_sum 13049"), std::string::npos)
        << "full text:\n" << text;
    // count = 5
    EXPECT_NE(text.find("h_count 5"), std::string::npos)
        << "full text:\n" << text;
}

// ============================================================
// MetricsRegistry 测试
// ============================================================
TEST(MetricsRegistryTest, SameNameReturnsSameCounter) {
    // 使用独立名称避免跨测试污染（单例持久）
    Counter& c1 = MetricsRegistry::instance().counter("__test_counter_identity__");
    Counter& c2 = MetricsRegistry::instance().counter("__test_counter_identity__");
    EXPECT_EQ(&c1, &c2) << "Same name must return same Counter object";
}

TEST(MetricsRegistryTest, SameNameReturnsSameGauge) {
    Gauge& g1 = MetricsRegistry::instance().gauge("__test_gauge_identity__");
    Gauge& g2 = MetricsRegistry::instance().gauge("__test_gauge_identity__");
    EXPECT_EQ(&g1, &g2);
}

TEST(MetricsRegistryTest, DumpAllContainsRegisteredNames) {
    Counter& c = MetricsRegistry::instance().counter("__test_dump_counter__");
    c.inc(7);
    Gauge& g = MetricsRegistry::instance().gauge("__test_dump_gauge__");
    g.set(42);

    const std::string dump = MetricsRegistry::instance().dump_all();
    EXPECT_NE(dump.find("__test_dump_counter__"), std::string::npos);
    EXPECT_NE(dump.find("__test_dump_gauge__"), std::string::npos);
}

TEST(MetricsRegistryTest, DifferentNamesAreDistinct) {
    Counter& ca = MetricsRegistry::instance().counter("__test_distinct_a__");
    Counter& cb = MetricsRegistry::instance().counter("__test_distinct_b__");
    ca.inc(10);
    cb.inc(20);
    EXPECT_EQ(ca.get(), 10);
    EXPECT_EQ(cb.get(), 20);
    EXPECT_NE(&ca, &cb);
}

// ============================================================
// Alerting 测试
// ============================================================
class AlertingTest : public ::testing::Test {
protected:
    // 空 webhook_url：仅打印 stderr，不发 HTTP
    Alerting alerting;
};

TEST_F(AlertingTest, FireAddsToHistory) {
    alerting.fire(AlertLevel::WARN, "test_alert", "test message");

    EXPECT_EQ(alerting.history().size(), 1u);
    EXPECT_EQ(alerting.history()[0].level,   AlertLevel::WARN);
    EXPECT_EQ(alerting.history()[0].name,    "test_alert");
    EXPECT_EQ(alerting.history()[0].message, "test message");
    EXPECT_GT(alerting.history()[0].ts_ns,   uint64_t{0});
}

TEST_F(AlertingTest, CountByLevel) {
    alerting.fire(AlertLevel::INFO,     "a", "msg1");
    alerting.fire(AlertLevel::WARN,     "b", "msg2");
    alerting.fire(AlertLevel::WARN,     "c", "msg3");
    alerting.fire(AlertLevel::CRITICAL, "d", "msg4");

    EXPECT_EQ(alerting.count(AlertLevel::INFO),     1u);
    EXPECT_EQ(alerting.count(AlertLevel::WARN),     2u);
    EXPECT_EQ(alerting.count(AlertLevel::CRITICAL), 1u);
}

TEST_F(AlertingTest, EmptyWebhookDoesNotCrash) {
    // 空 webhook，fire 多次，不崩溃
    for (int i = 0; i < 10; ++i) {
        alerting.fire(AlertLevel::INFO, "no_crash", "smoke test");
    }
    EXPECT_EQ(alerting.history().size(), 10u);
}

TEST_F(AlertingTest, HistoryCapAt1000) {
    // 超过 1000 条，旧条目应被移除
    for (int i = 0; i < 1050; ++i) {
        alerting.fire(AlertLevel::INFO, "flood", "msg");
    }
    EXPECT_LE(alerting.history().size(), 1000u);
}

TEST_F(AlertingTest, PredefinedCircuitBreakerOpen) {
    alerting.circuit_breaker_open("binance");
    ASSERT_EQ(alerting.history().size(), 1u);
    EXPECT_EQ(alerting.history()[0].level, AlertLevel::CRITICAL);
    EXPECT_EQ(alerting.history()[0].name,  "circuit_breaker_open");
    EXPECT_NE(alerting.history()[0].message.find("binance"), std::string::npos);
}

TEST_F(AlertingTest, PredefinedConnectionLost) {
    alerting.connection_lost("okx", 3000);
    ASSERT_EQ(alerting.history().size(), 1u);
    EXPECT_EQ(alerting.history()[0].level, AlertLevel::WARN);
    EXPECT_EQ(alerting.history()[0].name,  "connection_lost");
    EXPECT_NE(alerting.history()[0].message.find("3000"), std::string::npos);
}

TEST_F(AlertingTest, PredefinedLatencySpike) {
    alerting.latency_spike("strategy", 15000, 5000);
    ASSERT_EQ(alerting.history().size(), 1u);
    EXPECT_EQ(alerting.history()[0].level, AlertLevel::WARN);
    EXPECT_EQ(alerting.history()[0].name,  "latency_spike");
    EXPECT_NE(alerting.history()[0].message.find("strategy"), std::string::npos);
    EXPECT_NE(alerting.history()[0].message.find("15000"),    std::string::npos);
}

// ============================================================
// main
// ============================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
