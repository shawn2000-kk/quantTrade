// src/infra/logger.cpp
#include "infra/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>

namespace hft {

Logger& Logger::instance() noexcept {
    static Logger inst;
    return inst;
}

void Logger::init(const std::string& log_dir,
                  spdlog::level::level_enum level) noexcept {
    if (initialized_) return;

    try {
        // ── 创建日志目录 ─────────────────────────────────────────
        std::filesystem::create_directories(log_dir);

        // ── 异步线程池：8192 槽，2 个 worker 线程 ────────────────
        spdlog::init_thread_pool(8192, 2);

        // ── File sink：rotating（每个文件最大 100MB，保留 7 份）──
        const std::string log_file = log_dir + "/hft.log";
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file,
            100ULL * 1024 * 1024,  // 100 MB per file
            7                       // 最多保留 7 份
        );
        file_sink->set_level(level);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%l] [%n] %v");
        sinks_.push_back(file_sink);

        // ── Console sink：仅在 Debug 构建启用（Release 裁掉）────
#ifndef NDEBUG
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(level);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%l] [%n] %v");
        sinks_.push_back(console_sink);
#endif

        // ── 默认 async logger ─────────────────────────────────────
        // async_overflow_policy::overrun_oldest：队列满时丢最旧，不阻塞热路径
        auto default_logger = std::make_shared<spdlog::async_logger>(
            "hft",
            sinks_.begin(), sinks_.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest
        );
        default_logger->set_level(level);
        spdlog::register_logger(default_logger);
        spdlog::set_default_logger(default_logger);

        initialized_ = true;

    } catch (...) {
        // 初始化失败时静默处理：后续 get() 返回 spdlog 内置 stderr fallback
    }
}

void Logger::flush() noexcept {
    try {
        spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) {
            l->flush();
        });
    } catch (...) {}
}

std::shared_ptr<spdlog::logger> Logger::get(const std::string& name) noexcept {
    // 先从注册表查找（O(1) 查找，无锁争用）
    auto logger = spdlog::get(name);
    if (logger) return logger;

    // 不存在则按相同 sink 懒惰创建
    try {
        if (!initialized_ || sinks_.empty()) {
            // 未初始化时返回 spdlog 内置 default logger（输出到 stderr）
            return spdlog::default_logger();
        }
        auto new_logger = std::make_shared<spdlog::async_logger>(
            name,
            sinks_.begin(), sinks_.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest
        );
        new_logger->set_level(spdlog::default_logger()->level());
        spdlog::register_logger(new_logger);
        return new_logger;
    } catch (...) {
        return spdlog::default_logger();
    }
}

} // namespace hft
