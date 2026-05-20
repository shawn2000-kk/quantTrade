#include "connectivity/session_manager.hpp"

#include <algorithm>
#include <chrono>

namespace hft {

// ── 生命周期 ──────────────────────────────────────────────────────

SessionManager::SessionManager(IGateway& gw,
                               uint32_t  heartbeat_interval_ms) noexcept
    : gw_(gw)
    , heartbeat_interval_ms_(heartbeat_interval_ms)
{}

SessionManager::~SessionManager() noexcept {
    stop();
}

void SessionManager::start() noexcept {
    // 幂等：已在运行则直接返回
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    state_.store(State::CONNECTING, std::memory_order_relaxed);
    thread_ = std::thread([this]() noexcept { heartbeat_loop(); });
}

void SessionManager::stop() noexcept {
    // 幂等：未在运行则直接返回
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // 唤醒心跳线程（可能正在退避 sleep 中）
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    state_.store(State::DISCONNECTED, std::memory_order_relaxed);
}

// ── 状态查询 ──────────────────────────────────────────────────────

SessionManager::State SessionManager::state() const noexcept {
    return state_.load(std::memory_order_relaxed);
}

uint64_t SessionManager::reconnect_count() const noexcept {
    return reconnect_count_.load(std::memory_order_relaxed);
}

// ── 心跳主循环 ────────────────────────────────────────────────────
//
// 正常连接时：每 heartbeat_interval_ms_ 检查一次，状态保持 CONNECTED。
// 断连时：切换为 RECONNECTING，计数递增，按指数退避等待后再次检查。
//   - 退避起始值：100 ms
//   - 退避上限：  30 000 ms（30 s）
//   - 策略：     sleep(backoff)；每次断连将 backoff×2；重连成功后重置
//
// 注意：所有 sleep 均通过条件变量等待，以便 stop() 能立即打断。

void SessionManager::heartbeat_loop() noexcept {
    static constexpr uint32_t kMinBackoffMs = 100u;
    static constexpr uint32_t kMaxBackoffMs = 30'000u;

    uint32_t backoff_ms = kMinBackoffMs;

    while (running_.load(std::memory_order_relaxed)) {
        if (gw_.is_connected()) {
            // ── 连接正常 ──────────────────────────────────────────
            state_.store(State::CONNECTED, std::memory_order_relaxed);
            backoff_ms = kMinBackoffMs;  // 重置退避计数器

            // 以 heartbeat_interval_ms_ 为周期等待，可被 stop() 打断
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk,
                         std::chrono::milliseconds(heartbeat_interval_ms_),
                         [this]() noexcept {
                             return !running_.load(std::memory_order_relaxed);
                         });
        } else {
            // ── 断连 ─────────────────────────────────────────────
            State prev = state_.load(std::memory_order_relaxed);
            if (prev != State::RECONNECTING) {
                state_.store(State::RECONNECTING, std::memory_order_relaxed);
            }
            reconnect_count_.fetch_add(1u, std::memory_order_relaxed);

            // 指数退避等待，同样可被 stop() 打断
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk,
                         std::chrono::milliseconds(backoff_ms),
                         [this]() noexcept {
                             return !running_.load(std::memory_order_relaxed);
                         });

            // 计算下一次退避时长（上限 30 s）
            backoff_ms = std::min(backoff_ms * 2u, kMaxBackoffMs);
        }
    }
}

} // namespace hft
