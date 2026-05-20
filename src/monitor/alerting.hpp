#pragma once

// ============================================================
// alerting.hpp — Webhook 告警
//
// - 同步写 history_（最多 1000 条），打印到 stderr
// - 若 webhook_url 非空，异步（detach thread）HTTP POST JSON
// - Linux 使用 POSIX socket 发送；macOS 下 HTTP 发送为 no-op
// ============================================================

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"

namespace hft {

// ------------------------------------------------------------
// AlertLevel
// ------------------------------------------------------------
enum class AlertLevel : uint8_t {
    INFO     = 0,
    WARN     = 1,
    CRITICAL = 2,
};

// ------------------------------------------------------------
// Alert — 一条告警记录
// ------------------------------------------------------------
struct Alert {
    AlertLevel  level;
    std::string name;
    std::string message;
    Timestamp   ts_ns;  // 纳秒时间戳（rdtsc_ns()）
};

// ------------------------------------------------------------
// Alerting
// ------------------------------------------------------------
class Alerting {
public:
    /// @param webhook_url HTTP POST 目标，如 "http://host:8080/alert"
    ///                    空字符串 → 仅打印到 stderr，不发 HTTP
    explicit Alerting(std::string webhook_url = "") noexcept;

    // 不可拷贝
    Alerting(const Alerting&)            = delete;
    Alerting& operator=(const Alerting&) = delete;

    // --------------------------------------------------------
    // 核心接口
    // --------------------------------------------------------

    /// 触发一条告警：记录到 history_，打印 stderr，可选 async HTTP POST
    void fire(AlertLevel level,
              std::string_view name,
              std::string_view message) noexcept;

    // --------------------------------------------------------
    // 预定义业务告警辅助方法
    // --------------------------------------------------------

    /// 熔断器打开
    void circuit_breaker_open(std::string_view exchange) noexcept;

    /// 连接断开
    void connection_lost(std::string_view exchange,
                         uint64_t         duration_ms) noexcept;

    /// 延迟毛刺（p99 超阈值）
    void latency_spike(std::string_view stage,
                       uint64_t         p99_ns,
                       uint64_t         threshold_ns) noexcept;

    // --------------------------------------------------------
    // 查询接口
    // --------------------------------------------------------

    [[nodiscard]] const std::vector<Alert>& history() const noexcept {
        return history_;
    }

    /// 按级别统计历史告警数量
    [[nodiscard]] size_t count(AlertLevel level) const noexcept;

private:
    static constexpr size_t kMaxHistory = 1000;

    void send_async(std::string json_body) noexcept;

    std::string        webhook_url_;
    mutable std::mutex mu_;
    std::vector<Alert> history_;
};

} // namespace hft
