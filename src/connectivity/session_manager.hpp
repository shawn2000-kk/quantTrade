#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "connectivity/gateway.hpp"

// ============================================================
// session_manager.hpp — 网关会话生命周期管理
//
// 启动独立心跳线程，以 heartbeat_interval_ms 的周期轮询
// IGateway::is_connected()。一旦检测到断连，立即切换到
// RECONNECTING 状态，并以指数退避（100ms→30s）重试计数。
//
// 线程安全：state_ 和 reconnect_count_ 均为 std::atomic，
// stop() 使用条件变量打断正在等待的心跳 sleep，保证快速退出。
// ============================================================

namespace hft {

class SessionManager {
public:
    /// 会话状态机
    enum class State : uint8_t {
        DISCONNECTED,   ///< 未启动或已停止
        CONNECTING,     ///< 已启动，尚未收到第一次 is_connected()==true
        CONNECTED,      ///< 心跳确认连接正常
        RECONNECTING,   ///< 检测到断连，正在等待重连
    };

    /// @param gw                   被管理的网关引用（生命周期须长于 SessionManager）
    /// @param heartbeat_interval_ms 正常连接时的心跳检测周期（毫秒）
    explicit SessionManager(IGateway& gw,
                            uint32_t heartbeat_interval_ms = 5000) noexcept;

    /// 析构时自动调用 stop()，确保心跳线程退出后再销毁对象。
    ~SessionManager() noexcept;

    // 禁止拷贝 / 移动（内部持有线程句柄）
    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&)                 = delete;
    SessionManager& operator=(SessionManager&&)      = delete;

    // ── 生命周期 ──────────────────────────────────────────────────

    /// 启动心跳线程。若已启动则为空操作（幂等）。
    void start() noexcept;

    /// 停止心跳线程并等待其退出。若未启动则为空操作（幂等）。
    /// 使用条件变量唤醒，不会因退避 sleep 阻塞过久。
    void stop() noexcept;

    // ── 状态查询 ─────────────────────────────────────────────────

    [[nodiscard]] State    state()           const noexcept;
    [[nodiscard]] uint64_t reconnect_count() const noexcept;

private:
    void heartbeat_loop() noexcept;

    IGateway&                gw_;
    uint32_t                 heartbeat_interval_ms_;

    std::atomic<State>       state_{State::DISCONNECTED};
    std::atomic<uint64_t>    reconnect_count_{0};
    std::atomic<bool>        running_{false};

    std::mutex               cv_mutex_;
    std::condition_variable  cv_;
    std::thread              thread_;
};

} // namespace hft
