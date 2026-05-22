#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include "connectivity/gateway.hpp"
#include "oms/order.hpp"

namespace hft {

// ── FIX 4.2/4.4 会话 ──────────────────────────────────────────────────
//
// 实现 IGateway，提供对真实交易所的 FIX 协议接入
//
// 会话状态机：
//   NOT_CONNECTED → LOGGING_ON → ACTIVE → LOGGING_OUT → NOT_CONNECTED
//
// 消息编码策略：
//   - 所有 FIX 字段用整数 Tag=Value 格式手工编码（无第三方库依赖）
//   - BeginString: FIX.4.2 或 FIX.4.4（由构造参数决定）
//   - NewOrderSingle (D)：side=1/2, ordType=1/2/3, price=价格, orderQty=数量
//   - OrderCancelRequest (F)：origClOrdID + ClOrdID
//   - Heartbeat (0), Logon (A), Logout (5) 按 FIX 规范发送
//
// 连接：
//   - 同步 TCP 连接（阻塞 connect）
//   - 发送线程（热路径）直接写 socket
//   - 接收在 run() 中处理（专用线程）

enum class FIXVersion : uint8_t { FIX42 = 0, FIX44 = 1 };

class FIXSession : public IGateway {
public:
    // ── 回调类型 ────────────────────────────────────────────────────
    using AckCallback  = void (*)(const OrderAck&,   void* ctx);
    using FillCallback = void (*)(const FillEvent&,  void* ctx);

    // host        : 交易所 FIX 网关地址，如 "192.168.1.10"
    // port        : FIX 端口，如 9001
    // sender_comp : SenderCompID（本方）
    // target_comp : TargetCompID（交易所）
    // version     : FIX 版本
    FIXSession(const std::string& host,
               uint16_t           port,
               const std::string& sender_comp,
               const std::string& target_comp,
               FIXVersion         version = FIXVersion::FIX42) noexcept;

    ~FIXSession() noexcept override;

    FIXSession(const FIXSession&)            = delete;
    FIXSession& operator=(const FIXSession&) = delete;

    // ── 控制接口 ─────────────────────────────────────────────────────
    // connect()：建立 TCP 连接并发送 Logon，返回是否成功
    [[nodiscard]] bool connect() noexcept;

    // run()：阻塞式接收循环（在独立线程调用），直到断连或 disconnect()
    void run() noexcept;

    // disconnect()：发送 Logout 并关闭连接
    void disconnect() noexcept;

    // ── 回调注册 ─────────────────────────────────────────────────────
    void set_ack_callback(AckCallback cb, void* ctx) noexcept {
        ack_cb_ = cb; ack_ctx_ = ctx;
    }
    void set_fill_callback(FillCallback cb, void* ctx) noexcept {
        fill_cb_ = cb; fill_ctx_ = ctx;
    }

    // ── IGateway 实现 ────────────────────────────────────────────────
    void     send_new_order(const OrderRequest& req) noexcept override;
    void     send_cancel(uint64_t client_order_id)   noexcept override;
    void     send_modify(const ModifyRequest& req)   noexcept override;
    bool     is_connected()     const noexcept override;
    uint64_t get_rtt_ns()       const noexcept override;
    std::string_view name()     const noexcept override { return "FIXSession"; }

private:
    enum class SessionState : uint8_t {
        NOT_CONNECTED = 0,
        LOGGING_ON    = 1,
        ACTIVE        = 2,
        LOGGING_OUT   = 3,
    };

    // ── FIX 消息编码 ──────────────────────────────────────────────────
    // 返回写入 buf 的字节数；buf 大小必须 ≥ 512 字节
    size_t encode_logon(uint8_t* buf, size_t cap) noexcept;
    size_t encode_logout(uint8_t* buf, size_t cap) noexcept;
    size_t encode_heartbeat(uint8_t* buf, size_t cap, const char* test_req_id = nullptr) noexcept;
    size_t encode_new_order(uint8_t* buf, size_t cap, const OrderRequest& req) noexcept;
    size_t encode_cancel(uint8_t* buf, size_t cap, uint64_t orig_cloid) noexcept;
    size_t encode_modify(uint8_t* buf, size_t cap, const ModifyRequest& req) noexcept;

    // 公共 header：BeginString + BodyLength(占位) + MsgType
    size_t write_header(uint8_t* buf, size_t cap, char msg_type) noexcept;
    // 补全 BodyLength + 计算并附加 Checksum，返回最终消息长度
    size_t finalize(uint8_t* buf, size_t body_start, size_t body_end, size_t cap) noexcept;

    // ── 接收 & 解析 ────────────────────────────────────────────────
    void parse_message(const uint8_t* buf, size_t len) noexcept;
    void handle_execution_report(const uint8_t* buf, size_t len) noexcept;
    void handle_heartbeat(const uint8_t* buf, size_t len) noexcept;

    bool send_raw(const uint8_t* buf, size_t len) noexcept;

    // ── 连接参数 ──────────────────────────────────────────────────────
    std::string host_;
    uint16_t    port_;
    std::string sender_comp_;
    std::string target_comp_;
    FIXVersion  version_;

    int      sockfd_{-1};
    uint32_t msg_seq_num_{1};    // 发送序列号（单调递增）

    std::atomic<SessionState>  state_{SessionState::NOT_CONNECTED};
    std::atomic<uint64_t>      last_rtt_ns_{0};
    std::atomic<uint64_t>      last_heartbeat_ts_{0};

    static constexpr uint32_t HEARTBEAT_INTERVAL_S = 30;

    AckCallback  ack_cb_{nullptr};
    void*        ack_ctx_{nullptr};
    FillCallback fill_cb_{nullptr};
    void*        fill_ctx_{nullptr};
};

// ── FIX 消息构建辅助函数（文件内可见，不暴露到头文件外）────────────────
// 将整数写成字符串追加到 buf，返回写入字节数
size_t fix_append_int(uint8_t* buf, size_t pos, size_t cap, int64_t val) noexcept;
size_t fix_append_str(uint8_t* buf, size_t pos, size_t cap,
                      const char* val, size_t len) noexcept;
size_t fix_append_field(uint8_t* buf, size_t pos, size_t cap,
                        int tag, int64_t val) noexcept;
size_t fix_append_field_str(uint8_t* buf, size_t pos, size_t cap,
                             int tag, const char* val, size_t len) noexcept;

}  // namespace hft
