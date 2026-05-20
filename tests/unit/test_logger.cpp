// tests/unit/test_logger.cpp — Logger 单元测试
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>

#include "infra/logger.hpp"

namespace {

// 每个测试使用独立的日志目录
const std::string kLogDir = "/tmp/hft_test_logger";

} // namespace

// ============================================================
// 测试：Logger::init 后能正常写日志（不崩溃）
// ============================================================
TEST(Logger, InitAndLogDoesNotCrash) {
    hft::Logger::instance().init(kLogDir, spdlog::level::debug);

    // 各级别宏均应编译通过、运行不崩溃
    EXPECT_NO_FATAL_FAILURE({
        HFT_LOG_DEBUG("test", "debug message instrument_id={}", 42);
        HFT_LOG_INFO ("test", "info  message price={}", 12345);
        HFT_LOG_WARN ("test", "warn  message qty={}",  100);
        HFT_LOG_ERROR("test", "error message reason={}", "test_error");
    });
}

// ============================================================
// 测试：flush 后日志文件存在
// ============================================================
TEST(Logger, FlushCreatesLogFile) {
    hft::Logger::instance().init(kLogDir, spdlog::level::debug);
    HFT_LOG_INFO("flush_test", "flushing now");
    hft::Logger::instance().flush();

    // 日志目录应已存在（init 时自动创建）
    EXPECT_TRUE(std::filesystem::exists(kLogDir))
        << "log directory should exist after init: " << kLogDir;

    // 至少有一个 .log 文件
    bool found_log = false;
    for (const auto& entry : std::filesystem::directory_iterator(kLogDir)) {
        if (entry.path().extension() == ".log") {
            found_log = true;
            break;
        }
    }
    EXPECT_TRUE(found_log) << "no .log file found in " << kLogDir;
}

// ============================================================
// 测试：多次调用 init 幂等（不重复初始化）
// ============================================================
TEST(Logger, InitIsIdempotent) {
    // 连续调用两次，应不崩溃、不重复创建线程池
    hft::Logger::instance().init(kLogDir, spdlog::level::info);
    hft::Logger::instance().init(kLogDir, spdlog::level::debug);  // 第二次应无效

    EXPECT_NO_FATAL_FAILURE({
        HFT_LOG_INFO("idempotent", "still works");
    });
}

// ============================================================
// 测试：不同 logger_name 得到不同 logger 对象
// ============================================================
TEST(Logger, DifferentNamesReturnDifferentLoggers) {
    hft::Logger::instance().init(kLogDir, spdlog::level::debug);

    auto l1 = hft::Logger::instance().get("strategy");
    auto l2 = hft::Logger::instance().get("market_data");
    auto l3 = hft::Logger::instance().get("strategy");  // 再次获取，应同一对象

    ASSERT_NE(l1, nullptr);
    ASSERT_NE(l2, nullptr);
    ASSERT_NE(l3, nullptr);
    EXPECT_EQ(l1, l3) << "same name should return same logger instance";
    EXPECT_NE(l1->name(), l2->name());
}

// ============================================================
// 测试：未初始化时调用宏不崩溃（fallback 到 spdlog default logger）
// ============================================================
TEST(Logger, MacrosWorkBeforeInit) {
    // 注意：这个测试可能在其他测试之后运行（Logger 已 init），
    // 主要验证宏本身的编译正确性和运行时安全性。
    EXPECT_NO_FATAL_FAILURE({
        HFT_LOG_INFO ("pre_init", "should not crash");
        HFT_LOG_WARN ("pre_init", "warn before init");
        HFT_LOG_ERROR("pre_init", "error before init");
    });
}

// ============================================================
// 测试：多线程并发写日志不崩溃
// ============================================================
TEST(Logger, ConcurrentLoggingDoesNotCrash) {
    hft::Logger::instance().init(kLogDir, spdlog::level::info);

    constexpr int kThreads  = 4;
    constexpr int kMessages = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kMessages; ++i) {
                HFT_LOG_INFO("concurrent", "thread={} msg={}", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    hft::Logger::instance().flush();
    EXPECT_TRUE(std::filesystem::exists(kLogDir));
}
