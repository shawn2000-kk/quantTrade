#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include "connectivity/gateway.hpp"
#include "oms/order.hpp"

namespace hft {

// ── 交易所私有二进制协议网关 ────────────────────────────────────────────
//
// 实现 IGateway，封装常见交易所私有二进制协议（如上交所 BINARY、中金所）
//
// 消息格式（自定义，可按实际交易所规范替换 Header/Body 定义）：
//   +--------+-----------+----------------+
//   | Header |  MsgType  |  Body (变长)   |
//   +--------+-----------+----------------+
//   Header (8B)：magic(2) + version(1) + msg_type(1) + body_len(2) + checksum(2)
//   Body 按 msg_type 不同而不同，所有字段小端序
//
// 性能设计：
//   - iovec 零拷贝发送（header + body 分开，避免 memcpy 组装）
//   - 发送路径不分配堆内存（PreAllocated 发送缓冲区）
//   - 接收路径按 Header.body_len 准确定长读取
//
// 线程安全：
//   - send_new_order / send_cancel / send_modify：单线程（GatewayThread）调用
//   - run()：在独立线程执行接收循环
//   - is_connected / get_rtt_ns：原子读，任意线程安全

class BinaryProtocolGateway : public IGateway {
public:
    // ── Wire 消息定义（与实际交易所对齐时替换此区域）──────────────
    static constexpr uint16_t MAGIC   = 0x4846u;  // "HF"
    static constexpr uint8_t  VERSION = 0x01u;

    enum MsgType : uint8_t {
        MSG_NEW_ORDER   = 0x01,
        MSG_CANCEL      = 0x02,
        MSG_MODIFY      = 0x03,
        MSG_ORDER_ACK   = 0x11,
        MSG_ORDER_FILL  = 0x12,
        MSG_ORDER_REJECT= 0x13,
        MSG_HEARTBEAT   = 0x20,
        MSG_LOGON       = 0x21,
        MSG_LOGOFF      = 0x22,
    };

#pragma pack(push, 1)
    struct MsgHeader {
        uint16_t magic;
        uint8_t  version;
        uint8_t  msg_type;
        uint16_t body_len;
        uint16_t checksum;   // 简单校验：header 前 6 字节之和
    };
    static_assert(sizeof(MsgHeader) == 8);

    struct NewOrderBody {
        uint64_t client_order_id;
        uint32_t instrument_id;
        uint8_t  side;        // 0=BUY 1=SELL
        uint8_t  order_type;  // 0=MARKET 1=LIMIT 2=IOC 3=FOK
        uint8_t  _pad[2];
        int64_t  price;
        int64_t  qty;
        uint64_t timestamp_ns;
    };
    static_assert(sizeof(NewOrderBody) == 40);

    struct CancelBody {
        uint64_t client_order_id;
        uint64_t exchange_order_id;  // 0 表示仅用 cloid
        uint32_t instrument_id;
        uint8_t  _pad[4];
    };
    static_assert(sizeof(CancelBody) == 24);

    struct ModifyBody {
        uint64_t client_order_id;
        int64_t  new_price;
        int64_t  new_qty;
    };
    static_assert(sizeof(ModifyBody) == 24);

    struct AckBody {
        uint64_t    client_order_id;
        uint64_t    exchange_order_id;
        uint8_t     status;     // 0=ACK 1=REJECT
        uint8_t     reject_reason;
        uint8_t     _pad[6];
        uint64_t    timestamp_ns;
    };
    static_assert(sizeof(AckBody) == 32);

    struct FillBody {
        uint64_t client_order_id;
        uint64_t exchange_order_id;
        uint32_t instrument_id;
        uint8_t  side;
        uint8_t  _pad[3];
        int64_t  fill_price;
        int64_t  fill_qty;
        int64_t  remaining_qty;
        uint64_t timestamp_ns;
    };
    static_assert(sizeof(FillBody) == 56);
#pragma pack(pop)

    // ── 构造 ──────────────────────────────────────────────────────────
    BinaryProtocolGateway(const std::string& host,
                          uint16_t           port,
                          const std::string& session_id) noexcept;

    ~BinaryProtocolGateway() noexcept override;

    BinaryProtocolGateway(const BinaryProtocolGateway&)            = delete;
    BinaryProtocolGateway& operator=(const BinaryProtocolGateway&) = delete;

    // ── 控制接口 ──────────────────────────────────────────────────────
    [[nodiscard]] bool connect() noexcept;
    void run() noexcept;
    void disconnect() noexcept;

    // ── 回调 ──────────────────────────────────────────────────────────
    using AckCallback  = void (*)(const OrderAck&,  void* ctx);
    using FillCallback = void (*)(const FillEvent&, void* ctx);

    void set_ack_callback(AckCallback cb, void* ctx) noexcept {
        ack_cb_ = cb; ack_ctx_ = ctx;
    }
    void set_fill_callback(FillCallback cb, void* ctx) noexcept {
        fill_cb_ = cb; fill_ctx_ = ctx;
    }

    // ── IGateway 实现 ─────────────────────────────────────────────────
    void     send_new_order(const OrderRequest& req) noexcept override;
    void     send_cancel(uint64_t client_order_id)   noexcept override;
    void     send_modify(const ModifyRequest& req)   noexcept override;
    bool     is_connected()  const noexcept override;
    uint64_t get_rtt_ns()    const noexcept override;
    std::string_view name()  const noexcept override { return "BinaryGateway"; }

private:
    bool send_msg(uint8_t msg_type, const void* body, uint16_t body_len) noexcept;
    void recv_loop() noexcept;
    void handle_ack(const AckBody& ack) noexcept;
    void handle_fill(const FillBody& fill) noexcept;

    static uint16_t calc_checksum(const MsgHeader& h) noexcept;

    std::string host_;
    uint16_t    port_;
    std::string session_id_;
    int         sockfd_{-1};

    std::atomic<bool>     connected_{false};
    std::atomic<uint64_t> last_rtt_ns_{0};

    AckCallback  ack_cb_{nullptr};
    void*        ack_ctx_{nullptr};
    FillCallback fill_cb_{nullptr};
    void*        fill_ctx_{nullptr};
};

}  // namespace hft
