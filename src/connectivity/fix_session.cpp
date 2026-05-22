#include "connectivity/fix_session.hpp"
#include "infra/rdtsc_clock.hpp"
#include "infra/logger.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cinttypes>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

namespace hft {

// ── FIX 字段编码辅助 ────────────────────────────────────────────────────

static constexpr char SOH = '\x01';  // FIX 字段分隔符

size_t fix_append_int(uint8_t* buf, size_t pos, size_t cap, int64_t val) noexcept {
    if (pos >= cap) return pos;
    char tmp[24];
    int n = std::snprintf(tmp, sizeof(tmp), "%" PRId64, val);
    if (n <= 0 || pos + static_cast<size_t>(n) >= cap) return pos;
    std::memcpy(buf + pos, tmp, static_cast<size_t>(n));
    return pos + static_cast<size_t>(n);
}

size_t fix_append_str(uint8_t* buf, size_t pos, size_t cap,
                      const char* val, size_t len) noexcept {
    if (pos + len >= cap) return pos;
    std::memcpy(buf + pos, val, len);
    return pos + len;
}

size_t fix_append_field(uint8_t* buf, size_t pos, size_t cap,
                        int tag, int64_t val) noexcept {
    // "TAG=VALUE\x01"
    pos = fix_append_int(buf, pos, cap, static_cast<int64_t>(tag));
    if (pos < cap) buf[pos++] = '=';
    pos = fix_append_int(buf, pos, cap, val);
    if (pos < cap) buf[pos++] = SOH;
    return pos;
}

size_t fix_append_field_str(uint8_t* buf, size_t pos, size_t cap,
                             int tag, const char* val, size_t len) noexcept {
    pos = fix_append_int(buf, pos, cap, static_cast<int64_t>(tag));
    if (pos < cap) buf[pos++] = '=';
    pos = fix_append_str(buf, pos, cap, val, len);
    if (pos < cap) buf[pos++] = SOH;
    return pos;
}

// ── FIXSession 构造 / 析构 ───────────────────────────────────────────────

FIXSession::FIXSession(const std::string& host, uint16_t port,
                       const std::string& sender_comp,
                       const std::string& target_comp,
                       FIXVersion version) noexcept
    : host_(host), port_(port)
    , sender_comp_(sender_comp), target_comp_(target_comp)
    , version_(version)
{}

FIXSession::~FIXSession() noexcept {
    disconnect();
}

// ── 连接 ────────────────────────────────────────────────────────────────

bool FIXSession::connect() noexcept {
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd_ < 0) return false;

    // 禁用 Nagle 算法（低延迟）
    int nodelay = 1;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // 解析主机名
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port_);

    if (::getaddrinfo(host_.c_str(), port_str, &hints, &res) != 0 || !res) {
        HFT_LOG_ERROR("fix_session", "getaddrinfo failed for {}", host_);
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    if (::connect(sockfd_, res->ai_addr, res->ai_addrlen) < 0) {
        HFT_LOG_ERROR("fix_session", "connect to {}:{} failed: {}",
                      host_, port_, std::strerror(errno));
        ::freeaddrinfo(res);
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    ::freeaddrinfo(res);

    state_.store(SessionState::LOGGING_ON, std::memory_order_release);

    // 发送 Logon
    uint8_t buf[512];
    size_t  len = encode_logon(buf, sizeof(buf));
    if (!send_raw(buf, len)) {
        ::close(sockfd_);
        sockfd_ = -1;
        state_.store(SessionState::NOT_CONNECTED, std::memory_order_release);
        return false;
    }

    state_.store(SessionState::ACTIVE, std::memory_order_release);
    HFT_LOG_INFO("fix_session", "connected to {}:{}", host_, port_);
    return true;
}

void FIXSession::disconnect() noexcept {
    if (state_.load(std::memory_order_acquire) == SessionState::ACTIVE) {
        uint8_t buf[256];
        size_t  len = encode_logout(buf, sizeof(buf));
        send_raw(buf, len);
        state_.store(SessionState::LOGGING_OUT, std::memory_order_release);
    }
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    state_.store(SessionState::NOT_CONNECTED, std::memory_order_release);
}

// ── run() — 接收循环 ─────────────────────────────────────────────────────

void FIXSession::run() noexcept {
    alignas(64) uint8_t rbuf[65536];
    size_t  rpos = 0;

    while (state_.load(std::memory_order_acquire) != SessionState::NOT_CONNECTED) {
        ssize_t n = ::recv(sockfd_, rbuf + rpos, sizeof(rbuf) - rpos - 1, 0);
        if (n <= 0) {
            HFT_LOG_WARN("fix_session", "recv returned {}, disconnecting", n);
            state_.store(SessionState::NOT_CONNECTED, std::memory_order_release);
            break;
        }
        rpos += static_cast<size_t>(n);
        rbuf[rpos] = '\0';

        // 简单消息边界：按 "10=xxx\x01" (Checksum) 定界
        size_t parsed = 0;
        while (parsed < rpos) {
            uint8_t* trailer = static_cast<uint8_t*>(
                std::memchr(rbuf + parsed, '\x01', rpos - parsed));
            if (!trailer) break;
            size_t msg_len = static_cast<size_t>(trailer - (rbuf + parsed)) + 1;

            // 完整消息 = 以 "10=" 结尾
            if (msg_len >= 7 && rbuf[parsed + msg_len - 8] == '1' &&
                rbuf[parsed + msg_len - 7] == '0' &&
                rbuf[parsed + msg_len - 6] == '=') {
                parse_message(rbuf + parsed, msg_len);
            }
            parsed += msg_len;
        }
        if (parsed > 0 && parsed <= rpos) {
            rpos -= parsed;
            std::memmove(rbuf, rbuf + parsed, rpos);
        }
    }
}

// ── IGateway 实现 ────────────────────────────────────────────────────────

void FIXSession::send_new_order(const OrderRequest& req) noexcept {
    uint8_t buf[512];
    size_t  len = encode_new_order(buf, sizeof(buf), req);
    send_raw(buf, len);
}

void FIXSession::send_cancel(uint64_t client_order_id) noexcept {
    uint8_t buf[256];
    size_t  len = encode_cancel(buf, sizeof(buf), client_order_id);
    send_raw(buf, len);
}

void FIXSession::send_modify(const ModifyRequest& req) noexcept {
    uint8_t buf[512];
    size_t  len = encode_modify(buf, sizeof(buf), req);
    send_raw(buf, len);
}

bool FIXSession::is_connected() const noexcept {
    return state_.load(std::memory_order_acquire) == SessionState::ACTIVE;
}

uint64_t FIXSession::get_rtt_ns() const noexcept {
    return last_rtt_ns_.load(std::memory_order_relaxed);
}

// ── 消息编码 ────────────────────────────────────────────────────────────

// FIX header：8=FIX.4.2 + 9=<BodyLength> + 35=<MsgType>
size_t FIXSession::write_header(uint8_t* buf, size_t cap, char msg_type) noexcept {
    size_t pos = 0;
    const char* ver = (version_ == FIXVersion::FIX44) ? "FIX.4.4" : "FIX.4.2";
    pos = fix_append_field_str(buf, pos, cap, 8, ver, 7);
    // Tag 9 (BodyLength) 先写占位符 "000000"
    const size_t body_len_pos = pos;
    pos = fix_append_field_str(buf, pos, cap, 9, "000000", 6);
    (void)body_len_pos;  // finalize() 时回填
    buf[pos-7] = '\0';   // 标记 BodyLength 位置（简化，实际回填时用 finalize）
    char mt[2] = {msg_type, '\0'};
    pos = fix_append_field_str(buf, pos, cap, 35, mt, 1);
    pos = fix_append_field_str(buf, pos, cap, 49,
                                sender_comp_.c_str(), sender_comp_.size());
    pos = fix_append_field_str(buf, pos, cap, 56,
                                target_comp_.c_str(), target_comp_.size());
    pos = fix_append_field(buf, pos, cap, 34, static_cast<int64_t>(msg_seq_num_++));
    return pos;
}

size_t FIXSession::finalize(uint8_t* buf, size_t /*body_start*/,
                            size_t body_end, size_t cap) noexcept {
    // 追加 Checksum（Tag 10）
    uint32_t sum = 0;
    for (size_t i = 0; i < body_end; ++i) sum += buf[i];
    char cs[4];
    std::snprintf(cs, sizeof(cs), "%03u", sum % 256);
    size_t pos = fix_append_field_str(buf, body_end, cap, 10, cs, 3);
    return pos;
}

size_t FIXSession::encode_logon(uint8_t* buf, size_t cap) noexcept {
    size_t pos = write_header(buf, cap, 'A');
    pos = fix_append_field(buf, pos, cap, 98, 0);   // EncryptMethod=None
    pos = fix_append_field(buf, pos, cap, 108, HEARTBEAT_INTERVAL_S);
    return finalize(buf, 0, pos, cap);
}

size_t FIXSession::encode_logout(uint8_t* buf, size_t cap) noexcept {
    size_t pos = write_header(buf, cap, '5');
    return finalize(buf, 0, pos, cap);
}

size_t FIXSession::encode_heartbeat(uint8_t* buf, size_t cap,
                                    const char* test_req_id) noexcept {
    size_t pos = write_header(buf, cap, '0');
    if (test_req_id) {
        pos = fix_append_field_str(buf, pos, cap, 112,
                                   test_req_id, std::strlen(test_req_id));
    }
    return finalize(buf, 0, pos, cap);
}

size_t FIXSession::encode_new_order(uint8_t* buf, size_t cap,
                                    const OrderRequest& req) noexcept {
    size_t pos = write_header(buf, cap, 'D');

    // ClOrdID (11)：使用 strategy_ts_ns 作为唯一 ID（实际应由 OMS 分配）
    char cloid[24];
    size_t cloid_len = static_cast<size_t>(
        std::snprintf(cloid, sizeof(cloid), "%" PRIu64, req.strategy_ts_ns));
    pos = fix_append_field_str(buf, pos, cap, 11, cloid, cloid_len);

    // Side (54)：1=Buy, 2=Sell
    pos = fix_append_field(buf, pos, cap, 54,
                           req.side == Side::BUY ? 1 : 2);

    // OrdType (40)：1=Market, 2=Limit, 3=Stop
    const int ord_type = (req.type == OrderType::MARKET) ? 1 : 2;
    pos = fix_append_field(buf, pos, cap, 40, ord_type);

    // Symbol (55)：用 instrument_id 代替（实际需映射到交易所代码）
    char sym[12];
    size_t sym_len = static_cast<size_t>(
        std::snprintf(sym, sizeof(sym), "%u", req.instrument_id));
    pos = fix_append_field_str(buf, pos, cap, 55, sym, sym_len);

    // Price (44) + OrderQty (38)
    if (req.type != OrderType::MARKET) {
        pos = fix_append_field(buf, pos, cap, 44, req.price);
    }
    pos = fix_append_field(buf, pos, cap, 38, req.qty);

    // TransactTime (60)：简化用 UTC timestamp
    pos = fix_append_field_str(buf, pos, cap, 60, "20260101-00:00:00", 17);

    return finalize(buf, 0, pos, cap);
}

size_t FIXSession::encode_cancel(uint8_t* buf, size_t cap,
                                 uint64_t orig_cloid) noexcept {
    size_t pos = write_header(buf, cap, 'F');

    char id[24];
    size_t id_len = static_cast<size_t>(
        std::snprintf(id, sizeof(id), "%" PRIu64, orig_cloid));
    pos = fix_append_field_str(buf, pos, cap, 41, id, id_len);  // OrigClOrdID
    pos = fix_append_field_str(buf, pos, cap, 11, id, id_len);  // ClOrdID（新）
    pos = fix_append_field_str(buf, pos, cap, 60, "20260101-00:00:00", 17);
    return finalize(buf, 0, pos, cap);
}

size_t FIXSession::encode_modify(uint8_t* buf, size_t cap,
                                 const ModifyRequest& req) noexcept {
    size_t pos = write_header(buf, cap, 'G');  // OrderCancelReplaceRequest

    char id[24];
    size_t id_len = static_cast<size_t>(
        std::snprintf(id, sizeof(id), "%" PRIu64, req.client_order_id));
    pos = fix_append_field_str(buf, pos, cap, 41, id, id_len);  // OrigClOrdID
    pos = fix_append_field_str(buf, pos, cap, 11, id, id_len);  // ClOrdID
    pos = fix_append_field(buf, pos, cap, 44, req.new_price);
    pos = fix_append_field(buf, pos, cap, 38, req.new_qty);
    pos = fix_append_field_str(buf, pos, cap, 60, "20260101-00:00:00", 17);
    return finalize(buf, 0, pos, cap);
}

// ── 消息解析 ────────────────────────────────────────────────────────────

static const uint8_t* fix_find_field(const uint8_t* buf, size_t len, int tag) noexcept {
    char prefix[8];
    int  plen = std::snprintf(prefix, sizeof(prefix), "\x01%d=", tag);
    if (plen <= 0) return nullptr;

    // 搜索 "\x01TAG=" 子串
    for (size_t i = 0; i + static_cast<size_t>(plen) < len; ++i) {
        if (std::memcmp(buf + i, prefix, static_cast<size_t>(plen)) == 0) {
            return buf + i + plen;  // 返回 value 起始位置
        }
    }
    return nullptr;
}

static int64_t fix_read_int(const uint8_t* val_ptr) noexcept {
    if (!val_ptr) return 0;
    int64_t result = 0;
    bool neg = (*val_ptr == '-');
    if (neg) ++val_ptr;
    while (*val_ptr && *val_ptr != SOH) {
        result = result * 10 + (*val_ptr - '0');
        ++val_ptr;
    }
    return neg ? -result : result;
}

void FIXSession::parse_message(const uint8_t* buf, size_t len) noexcept {
    const uint8_t* msg_type_ptr = fix_find_field(buf, len, 35);
    if (!msg_type_ptr) return;

    const char msg_type = static_cast<char>(*msg_type_ptr);

    switch (msg_type) {
        case '8':  // ExecutionReport
            handle_execution_report(buf, len);
            break;
        case '0':  // Heartbeat
            handle_heartbeat(buf, len);
            break;
        case 'A':  // Logon ack
            state_.store(SessionState::ACTIVE, std::memory_order_release);
            HFT_LOG_INFO("fix_session", "logon accepted");
            break;
        case '5':  // Logout
            state_.store(SessionState::NOT_CONNECTED, std::memory_order_release);
            HFT_LOG_INFO("fix_session", "received logout");
            break;
        default:
            break;
    }
}

void FIXSession::handle_execution_report(const uint8_t* buf, size_t len) noexcept {
    // ExecType (150)：0=New, 1=PartialFill, 2=Fill, 8=Rejected
    const uint8_t* exec_type = fix_find_field(buf, len, 150);
    if (!exec_type) return;

    const int64_t cloid          = fix_read_int(fix_find_field(buf, len, 11));
    const int64_t exchange_oid   = fix_read_int(fix_find_field(buf, len, 37));
    const char    et             = static_cast<char>(*exec_type);

    if (et == '0' || et == '8') {
        // ACK 或 REJECT
        OrderAck ack{};
        ack.client_order_id   = static_cast<uint64_t>(cloid);
        ack.exchange_order_id = static_cast<uint64_t>(exchange_oid);
        ack.status = (et == '0') ? OrderStatus::NEW : OrderStatus::REJECTED;
        ack.ack_ts_ns = rdtsc_ns();
        if (ack_cb_) ack_cb_(ack, ack_ctx_);
    } else if (et == '1' || et == '2') {
        // 部分成交 / 全成交
        FillEvent fill{};
        fill.client_order_id   = static_cast<uint64_t>(cloid);
        fill.exchange_order_id = static_cast<uint64_t>(exchange_oid);
        fill.instrument_id     = static_cast<uint32_t>(fix_read_int(fix_find_field(buf, len, 55)));
        fill.fill_price        = fix_read_int(fix_find_field(buf, len, 31));   // LastPx
        fill.fill_qty          = fix_read_int(fix_find_field(buf, len, 32));   // LastQty
        fill.remaining_qty     = fix_read_int(fix_find_field(buf, len, 151));  // LeavesQty
        fill.fill_ts_ns        = rdtsc_ns();
        const int64_t side     = fix_read_int(fix_find_field(buf, len, 54));
        fill.side = (side == 1) ? Side::BUY : Side::SELL;
        if (fill_cb_) fill_cb_(fill, fill_ctx_);
    }
}

void FIXSession::handle_heartbeat(const uint8_t* /*buf*/, size_t /*len*/) noexcept {
    const uint64_t now = rdtsc_ns();
    const uint64_t last = last_heartbeat_ts_.exchange(now, std::memory_order_relaxed);
    if (last > 0) {
        last_rtt_ns_.store(now - last, std::memory_order_relaxed);
    }
}

// ── 底层发送 ────────────────────────────────────────────────────────────

bool FIXSession::send_raw(const uint8_t* buf, size_t len) noexcept {
    if (sockfd_ < 0 || len == 0) return false;
    ssize_t sent = ::send(sockfd_, buf, len, MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(len);
}

}  // namespace hft
