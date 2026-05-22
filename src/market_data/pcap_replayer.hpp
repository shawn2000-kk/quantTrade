#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <atomic>
#include "common/types.hpp"
#include "market_data/market_data_types.hpp"
#include "market_data/normalizer.hpp"

namespace hft {

// ── PcapReplayer ──────────────────────────────────────────────────────────
//
// 职责：读取 PCAP 文件，按时间顺序回放行情事件（BBOEvent / DepthEvent）
//
// 支持两种时序模式：
//   REALTIME  — 保持原始时间间隔（基于 PCAP 时间戳 + steady_clock 实现）
//   FASTEST   — 忽略时间戳，以最快速度回放（压测/单元测试常用）
//
// PCAP 格式：
//   Global header (24B) + N × [Packet header (16B) + Packet data]
//   假设 payload = Ethernet(14B) + IP(20B) + UDP(8B) + WireBBO/WireDepth
//   解析时跳过二三层头（offset = 42 字节）
//
// 线程模型：
//   replay()：阻塞调用，在调用线程（通常为 BacktestThread）上运行
//   stop()：可从任意线程调用，触发 replay() 尽快退出

class PcapReplayer {
public:
    enum class Mode { REALTIME, FASTEST };

    // ── 回调类型定义 ──────────────────────────────────────────────────
    using BboCallback   = std::function<void(const BBOEvent&)>;
    using DepthCallback = std::function<void(const DepthEvent&)>;

    explicit PcapReplayer(const std::string& pcap_path,
                          ExchangeType       exchange = ExchangeType::INTERNAL,
                          Mode               mode     = Mode::FASTEST) noexcept;
    ~PcapReplayer() noexcept;

    PcapReplayer(const PcapReplayer&)            = delete;
    PcapReplayer& operator=(const PcapReplayer&) = delete;

    // ── 回调注册（replay 前调用）─────────────────────────────────────
    void set_bbo_callback(BboCallback cb) noexcept   { bbo_cb_   = std::move(cb); }
    void set_depth_callback(DepthCallback cb) noexcept { depth_cb_ = std::move(cb); }

    // ── 控制接口 ─────────────────────────────────────────────────────
    // replay()：阻塞，返回后说明文件读完或 stop() 被调用
    // 返回 true = 正常结束，false = 文件打开/格式错误
    [[nodiscard]] bool replay() noexcept;

    // stop()：线程安全，通知 replay() 退出
    void stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    // ── 统计 ─────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t packets_replayed()  const noexcept { return pkts_replayed_; }
    [[nodiscard]] uint64_t messages_replayed() const noexcept { return msgs_replayed_; }
    [[nodiscard]] uint64_t parse_errors()      const noexcept { return parse_errors_; }

private:
    // PCAP 文件头（24 字节）
    struct PcapGlobalHeader {
        uint32_t magic_number;   // 0xa1b2c3d4 (LE) 或 0xd4c3b2a1 (BE)
        uint16_t version_major;
        uint16_t version_minor;
        int32_t  thiszone;
        uint32_t sigfigs;
        uint32_t snaplen;
        uint32_t network;        // 1 = LINKTYPE_ETHERNET
    };

    // PCAP 包头（16 字节）
    struct PcapPktHeader {
        uint32_t ts_sec;
        uint32_t ts_usec;        // 微秒（magic=0xa1b2c3d4）或纳秒（magic=0xa1b23c4d）
        uint32_t incl_len;       // 截取长度
        uint32_t orig_len;       // 原始长度
    };

    bool open_file() noexcept;
    void close_file() noexcept;
    void dispatch_payload(const uint8_t* payload, size_t len,
                          uint64_t pkt_ts_ns) noexcept;

    std::string  pcap_path_;
    ExchangeType exchange_;
    Mode         mode_;

    // FILE* 而非 ifstream：fread/fseek 更高效
    void*  fp_{nullptr};
    bool   nano_ts_{false};   // true = pcap nanosecond timestamps
    bool   swap_endian_{false};

    std::atomic<bool> running_{false};

    BboCallback   bbo_cb_;
    DepthCallback depth_cb_;

    uint64_t pkts_replayed_{0};
    uint64_t msgs_replayed_{0};
    uint64_t parse_errors_{0};
};

}  // namespace hft
