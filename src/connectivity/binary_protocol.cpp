#include "connectivity/binary_protocol.hpp"
#include "infra/rdtsc_clock.hpp"
#include "infra/logger.hpp"

#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#ifdef __linux__
#  include <sys/uio.h>
#endif

namespace hft {

// ── 校验和计算 ─────────────────────────────────────────────────────────

uint16_t BinaryProtocolGateway::calc_checksum(const MsgHeader& h) noexcept {
    // header 前 6 字节（magic+version+msg_type+body_len）之和
    const auto* p = reinterpret_cast<const uint8_t*>(&h);
    uint16_t sum = 0;
    for (int i = 0; i < 6; ++i) sum += p[i];
    return sum;
}

// ── 构造 / 析构 ────────────────────────────────────────────────────────

BinaryProtocolGateway::BinaryProtocolGateway(const std::string& host,
                                             uint16_t           port,
                                             const std::string& session_id) noexcept
    : host_(host), port_(port), session_id_(session_id)
{}

BinaryProtocolGateway::~BinaryProtocolGateway() noexcept {
    disconnect();
}

// ── 连接 ──────────────────────────────────────────────────────────────

bool BinaryProtocolGateway::connect() noexcept {
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd_ < 0) return false;

    int nodelay = 1;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port_);

    if (::getaddrinfo(host_.c_str(), port_str, &hints, &res) != 0 || !res) {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    if (::connect(sockfd_, res->ai_addr, res->ai_addrlen) < 0) {
        HFT_LOG_ERROR("binary_gw", "connect {}:{} failed: {}",
                      host_, port_, std::strerror(errno));
        ::freeaddrinfo(res);
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    ::freeaddrinfo(res);

    // 发送 Logon 握手（只含 session_id，无 body）
    if (!send_msg(MSG_LOGON, session_id_.c_str(),
                  static_cast<uint16_t>(session_id_.size()))) {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    connected_.store(true, std::memory_order_release);
    HFT_LOG_INFO("binary_gw", "connected to {}:{} session={}",
                 host_, port_, session_id_);
    return true;
}

void BinaryProtocolGateway::disconnect() noexcept {
    if (connected_.load(std::memory_order_acquire)) {
        send_msg(MSG_LOGOFF, nullptr, 0);
        connected_.store(false, std::memory_order_release);
    }
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
}

// ── run() — 接收循环 ───────────────────────────────────────────────────

void BinaryProtocolGateway::run() noexcept {
    alignas(64) uint8_t rbuf[65536];
    size_t rpos = 0;

    while (connected_.load(std::memory_order_acquire)) {
        ssize_t n = ::recv(sockfd_, rbuf + rpos, sizeof(rbuf) - rpos, 0);
        if (n <= 0) {
            HFT_LOG_WARN("binary_gw", "recv returned {}", n);
            connected_.store(false, std::memory_order_release);
            break;
        }
        rpos += static_cast<size_t>(n);

        while (rpos >= sizeof(MsgHeader)) {
            MsgHeader hdr{};
            std::memcpy(&hdr, rbuf, sizeof(MsgHeader));

            if (hdr.magic != MAGIC) {
                HFT_LOG_ERROR("binary_gw", "bad magic {:04x}", hdr.magic);
                connected_.store(false, std::memory_order_release);
                return;
            }

            const size_t msg_len = sizeof(MsgHeader) + hdr.body_len;
            if (rpos < msg_len) break;  // 等待更多数据

            const uint8_t* body = rbuf + sizeof(MsgHeader);

            switch (hdr.msg_type) {
                case MSG_ORDER_ACK:
                    if (hdr.body_len >= sizeof(AckBody)) {
                        AckBody ack{};
                        std::memcpy(&ack, body, sizeof(AckBody));
                        handle_ack(ack);
                    }
                    break;
                case MSG_ORDER_FILL:
                    if (hdr.body_len >= sizeof(FillBody)) {
                        FillBody fill{};
                        std::memcpy(&fill, body, sizeof(FillBody));
                        handle_fill(fill);
                    }
                    break;
                case MSG_HEARTBEAT: {
                    const uint64_t now = rdtsc_ns();
                    last_rtt_ns_.store(now - hdr.body_len, std::memory_order_relaxed);
                    // 回 pong
                    send_msg(MSG_HEARTBEAT, nullptr, 0);
                    break;
                }
                default:
                    break;
            }

            rpos -= msg_len;
            std::memmove(rbuf, rbuf + msg_len, rpos);
        }
    }
}

// ── IGateway 实现 ──────────────────────────────────────────────────────

void BinaryProtocolGateway::send_new_order(const OrderRequest& req) noexcept {
    NewOrderBody body{};
    body.client_order_id = req.strategy_ts_ns;  // 临时用 ts 作 cloid
    body.instrument_id   = req.instrument_id;
    body.side            = static_cast<uint8_t>(req.side);
    body.order_type      = static_cast<uint8_t>(req.type);
    body.price           = req.price;
    body.qty             = req.qty;
    body.timestamp_ns    = rdtsc_ns();
    send_msg(MSG_NEW_ORDER, &body, sizeof(body));
}

void BinaryProtocolGateway::send_cancel(uint64_t client_order_id) noexcept {
    CancelBody body{};
    body.client_order_id = client_order_id;
    send_msg(MSG_CANCEL, &body, sizeof(body));
}

void BinaryProtocolGateway::send_modify(const ModifyRequest& req) noexcept {
    ModifyBody body{};
    body.client_order_id = req.client_order_id;
    body.new_price       = req.new_price;
    body.new_qty         = req.new_qty;
    send_msg(MSG_MODIFY, &body, sizeof(body));
}

bool BinaryProtocolGateway::is_connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

uint64_t BinaryProtocolGateway::get_rtt_ns() const noexcept {
    return last_rtt_ns_.load(std::memory_order_relaxed);
}

// ── 底层发送（iovec 零拷贝）─────────────────────────────────────────────

bool BinaryProtocolGateway::send_msg(uint8_t msg_type,
                                     const void* body,
                                     uint16_t body_len) noexcept {
    MsgHeader hdr{};
    hdr.magic    = MAGIC;
    hdr.version  = VERSION;
    hdr.msg_type = msg_type;
    hdr.body_len = body_len;
    hdr.checksum = calc_checksum(hdr);

#ifdef __linux__
    // iovec 零拷贝：header + body 一次 writev
    iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = const_cast<void*>(body);
    iov[1].iov_len  = body_len;
    int iovcnt = (body && body_len > 0) ? 2 : 1;
    ssize_t sent = ::writev(sockfd_, iov, iovcnt);
    return sent == static_cast<ssize_t>(sizeof(hdr) + body_len);
#else
    // macOS: 两次 send（仍然有效，但多一次系统调用）
    if (::send(sockfd_, &hdr, sizeof(hdr), MSG_NOSIGNAL)
            != static_cast<ssize_t>(sizeof(hdr))) return false;
    if (body && body_len > 0) {
        if (::send(sockfd_, body, body_len, MSG_NOSIGNAL)
                != static_cast<ssize_t>(body_len)) return false;
    }
    return true;
#endif
}

// ── 事件处理 ──────────────────────────────────────────────────────────

void BinaryProtocolGateway::handle_ack(const AckBody& ack) noexcept {
    OrderAck out{};
    out.client_order_id   = ack.client_order_id;
    out.exchange_order_id = ack.exchange_order_id;
    out.status = (ack.status == 0) ? OrderStatus::NEW : OrderStatus::REJECTED;
    out.ack_ts_ns = ack.timestamp_ns;
    if (ack_cb_) ack_cb_(out, ack_ctx_);
}

void BinaryProtocolGateway::handle_fill(const FillBody& fill) noexcept {
    FillEvent out{};
    out.client_order_id   = fill.client_order_id;
    out.exchange_order_id = fill.exchange_order_id;
    out.instrument_id     = fill.instrument_id;
    out.side              = static_cast<Side>(fill.side);
    out.fill_price        = fill.fill_price;
    out.fill_qty          = fill.fill_qty;
    out.remaining_qty     = fill.remaining_qty;
    out.fill_ts_ns        = fill.timestamp_ns;
    if (fill_cb_) fill_cb_(out, fill_ctx_);
}

}  // namespace hft
