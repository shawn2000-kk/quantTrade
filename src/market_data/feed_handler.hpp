#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include "common/types.hpp"
#include "infra/spsc_queue.hpp"
#include "market_data/market_data_types.hpp"
#include "market_data/normalizer.hpp"

namespace hft {

// ── FeedHandler ──────────────────────────────────────────────────────────
//
// 职责：UDP 组播接收 + 解析 + 推送 BBOEvent 到热路径 SPSC 队列
//
// 线程模型：
//   - run() 在独立的 MarketDataThread 上调用（绑核 2）
//   - push 写入 bbo_queue_，策略线程 pop 消费
//
// Linux：使用 recvmmsg() 批量收包，每批最多 BATCH_SIZE(64) 包
// macOS：recvmsg() 逐包，接口保持一致
//
// 停止方式：调用 stop()（原子写，run() 下一轮循环退出）

class FeedHandler {
public:
    static constexpr size_t BATCH_SIZE    = 64;    // recvmmsg 每批最多包数
    static constexpr size_t QUEUE_DEPTH   = 4096;  // SPSC 队列深度（2 的幂）
    static constexpr size_t MAX_PKT_BYTES = 2048;  // 单包最大字节数

    // ── 构造 / 析构 ────────────────────────────────────────────────────
    // mcast_addr : 组播地址，如 "239.0.0.1"
    // mcast_port : 端口号，如 9001
    // iface      : 绑定的本地网口名，如 "eth0"，空串=默认路由
    // exchange   : 行情格式（影响 Normalizer 解析路径）
    explicit FeedHandler(const std::string& mcast_addr,
                         uint16_t           mcast_port,
                         const std::string& iface    = "",
                         ExchangeType       exchange = ExchangeType::INTERNAL) noexcept;
    ~FeedHandler() noexcept;

    FeedHandler(const FeedHandler&)            = delete;
    FeedHandler& operator=(const FeedHandler&) = delete;

    // ── 控制接口 ──────────────────────────────────────────────────────
    // open()：创建 socket、加入组播组，失败返回 false
    [[nodiscard]] bool open() noexcept;

    // run()：阻塞式收包循环，直到 stop() 被调用
    void run() noexcept;

    // stop()：线程安全，可从任意线程调用
    void stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    // ── 数据接口（消费者线程）────────────────────────────────────────
    // pop_bbo：取出一条 BBOEvent，空时返回 false
    bool pop_bbo(BBOEvent& out) noexcept { return bbo_queue_.pop(out); }

    // 注册回调（可选，优先级低于 SPSC 队列）：
    // 若同时注册了回调，每条解析成功的 BBO 都会被调用（callback 在 MarketDataThread 执行）
    void set_bbo_callback(std::function<void(const BBOEvent&)> cb) noexcept {
        bbo_callback_ = std::move(cb);
    }

    // ── 统计 ──────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t rx_packets()  const noexcept { return rx_pkts_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t rx_messages() const noexcept { return rx_msgs_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped()     const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    void recv_loop_linux() noexcept;   // recvmmsg 批量路径
    void recv_loop_posix() noexcept;   // recvmsg 逐包路径（macOS / 兜底）
    void process_packet(const uint8_t* buf, size_t len, uint64_t local_ts) noexcept;

    std::string  mcast_addr_;
    uint16_t     mcast_port_;
    std::string  iface_;
    ExchangeType exchange_;
    int          sockfd_{-1};

    std::atomic<bool> running_{false};

    SPSCQueue<BBOEvent, QUEUE_DEPTH> bbo_queue_;

    std::function<void(const BBOEvent&)> bbo_callback_;

    alignas(64) std::atomic<uint64_t> rx_pkts_{0};
    alignas(64) std::atomic<uint64_t> rx_msgs_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
};

}  // namespace hft
