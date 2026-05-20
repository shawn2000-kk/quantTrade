// src/infra/logger.hpp — 异步日志封装（基于 spdlog）
//
// 特性：
//   - 单例（Meyer's singleton，线程安全）
//   - 异步 sink：8192 槽环形 buffer，2 个后台 worker 线程
//   - 双 sink：rotating file（保留 7 份）+ 彩色 console（Debug 构建）
//   - 日志格式：[2026-05-20 10:00:00.123456] [WARN] [market_data] message
//   - Release 构建下 DEBUG 日志被编译期裁掉（SPDLOG_ACTIVE_LEVEL 宏）
//   - namespace hft
//
// 宏用法：
//   HFT_LOG_INFO("market_data", "price={} qty={}", price, qty)
#pragma once

// ── 编译期日志级别控制 ─────────────────────────────────────────────────
// Release 构建（NDEBUG 已定义）：INFO 及以上；DEBUG 日志被编译期裁掉。
// Debug 构建：DEBUG 及以上（全部输出）。
// 外部 CMake 可通过 -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE 覆盖。
#ifndef SPDLOG_ACTIVE_LEVEL
#  ifdef NDEBUG
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#  else
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#  endif
#endif

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <vector>

namespace hft {

// ============================================================
// Logger — 基于 spdlog 的异步单例日志
// ============================================================
class Logger {
public:
    /// Meyer's singleton，线程安全。
    static Logger& instance() noexcept;

    // 禁止拷贝 / 移动
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;

    /// 初始化：创建异步 thread_pool，配置 file + console sink。
    /// 幂等：多次调用只有第一次生效。
    /// @param log_dir  日志文件目录，不存在时自动创建。
    /// @param level    最低输出级别（debug / info / warn / error / critical）。
    void init(const std::string& log_dir,
              spdlog::level::level_enum level) noexcept;

    /// 程序退出前调用，刷新所有 sink 确保日志落盘。
    void flush() noexcept;

    /// 按 name 获取具名 logger（不存在则按相同 sink 懒惰创建）。
    /// 返回 shared_ptr，直接传给 SPDLOG_LOGGER_* 系列宏。
    std::shared_ptr<spdlog::logger> get(const std::string& name) noexcept;

private:
    Logger() noexcept = default;

    bool                       initialized_{false};
    std::vector<spdlog::sink_ptr> sinks_;
};

} // namespace hft

// ── 日志宏（避免未命中 level 时的格式化开销）────────────────────────────
// logger_name    : 字符串字面量或 std::string，标识来源模块
// fmt_and_args   : fmtlib 格式字符串 [, 参数...]（整体作为 __VA_ARGS__ 传递）
//
// 用法示例：
//   HFT_LOG_INFO("market_data", "price={} qty={}", price, qty)
//   HFT_LOG_WARN("risk", "rate limit hit")
//
// Release 构建中 HFT_LOG_DEBUG 宏展开为空语句（SPDLOG_ACTIVE_LEVEL 控制）。

#define HFT_LOG_DEBUG(logger_name, ...) \
    SPDLOG_LOGGER_DEBUG(hft::Logger::instance().get(logger_name), __VA_ARGS__)

#define HFT_LOG_INFO(logger_name, ...) \
    SPDLOG_LOGGER_INFO(hft::Logger::instance().get(logger_name), __VA_ARGS__)

#define HFT_LOG_WARN(logger_name, ...) \
    SPDLOG_LOGGER_WARN(hft::Logger::instance().get(logger_name), __VA_ARGS__)

#define HFT_LOG_ERROR(logger_name, ...) \
    SPDLOG_LOGGER_ERROR(hft::Logger::instance().get(logger_name), __VA_ARGS__)
