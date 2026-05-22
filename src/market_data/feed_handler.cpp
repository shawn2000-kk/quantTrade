#include "market_data/feed_handler.hpp"
#include "infra/rdtsc_clock.hpp"
#include "infra/logger.hpp"

#include <cstring>
#include <cerrno>

#ifdef __linux__
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <unistd.h>
#else
// macOS / BSD
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <unistd.h>
#endif

namespace hft {

FeedHandler::FeedHandler(const std::string& mcast_addr,
                         uint16_t           mcast_port,
                         const std::string& iface,
                         ExchangeType       exchange) noexcept
    : mcast_addr_(mcast_addr)
    , mcast_port_(mcast_port)
    , iface_(iface)
    , exchange_(exchange)
{}

FeedHandler::~FeedHandler() noexcept {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
}

bool FeedHandler::open() noexcept {
    sockfd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd_ < 0) {
        HFT_LOG_ERROR("feed_handler", "socket() failed: {}", std::strerror(errno));
        return false;
    }

    // SO_REUSEADDR / SO_REUSEPORT：允许多进程同时加入同一组播组
    int yes = 1;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        HFT_LOG_WARN("feed_handler", "SO_REUSEADDR failed: {}", std::strerror(errno));
    }
#ifdef SO_REUSEPORT
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) < 0) {
        HFT_LOG_WARN("feed_handler", "SO_REUSEPORT failed: {}", std::strerror(errno));
    }
#endif

    // 接收缓冲区设为 64MB，降低包丢失率
    int rcvbuf = 64 * 1024 * 1024;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        HFT_LOG_WARN("feed_handler", "SO_RCVBUF failed: {}", std::strerror(errno));
    }

    // 绑定到组播端口
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(mcast_port_);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sockfd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        HFT_LOG_ERROR("feed_handler", "bind() failed: {}", std::strerror(errno));
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 判断是否为组播地址（224.0.0.0/4，即首字节 224-239）
    uint32_t addr_n = ::inet_addr(mcast_addr_.c_str());
    uint32_t addr_h = ntohl(addr_n);
    const bool is_mcast = ((addr_h >> 28) == 0xE);  // 1110 xxxx

    if (is_mcast) {
        // 加入组播组
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = addr_n;

        if (!iface_.empty()) {
            ifreq ifr{};
            std::strncpy(ifr.ifr_name, iface_.c_str(), IFNAMSIZ - 1);
#ifdef SIOCGIFADDR
            if (::ioctl(sockfd_, SIOCGIFADDR, &ifr) == 0) {
                mreq.imr_interface =
                    reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr;
            } else {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            }
#else
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#endif
        } else {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        }

        if (::setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &mreq, sizeof(mreq)) < 0) {
            HFT_LOG_ERROR("feed_handler", "IP_ADD_MEMBERSHIP failed: {}",
                          std::strerror(errno));
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }

        // 开启本地回环，方便同机 mock_feed 测试
        int loop = 1;
        ::setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        HFT_LOG_INFO("feed_handler", "joined multicast {}:{}", mcast_addr_, mcast_port_);
    } else {
        // 单播模式：直接绑定指定地址（用于开发测试，如 127.0.0.1）
        HFT_LOG_INFO("feed_handler", "unicast mode {}:{}", mcast_addr_, mcast_port_);
    }
    return true;
}

// ── 处理单个 UDP 包 ─────────────────────────────────────────────────────

void FeedHandler::process_packet(const uint8_t* buf, size_t len,
                                 uint64_t local_ts) noexcept {
    BBOEvent events[BATCH_SIZE];
    size_t n = Normalizer::parse_batch(buf, len, events, BATCH_SIZE, exchange_);

    for (size_t i = 0; i < n; ++i) {
        events[i].local_ts_ns = local_ts;

        if (bbo_callback_) bbo_callback_(events[i]);

        if (!bbo_queue_.push(events[i])) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    rx_msgs_.fetch_add(n, std::memory_order_relaxed);
}

// ── 收包主循环 ──────────────────────────────────────────────────────────

void FeedHandler::run() noexcept {
    if (sockfd_ < 0) {
        HFT_LOG_ERROR("feed_handler", "run() called before open()");
        return;
    }
    running_.store(true, std::memory_order_relaxed);

#ifdef __linux__
    recv_loop_linux();
#else
    recv_loop_posix();
#endif
}

// Linux: recvmmsg 批量收包（一次系统调用收 BATCH_SIZE 包）
void FeedHandler::recv_loop_linux() noexcept {
#ifdef __linux__
    alignas(64) uint8_t pkt_bufs[BATCH_SIZE][MAX_PKT_BYTES];
    iovec        iovecs[BATCH_SIZE];
    mmsghdr      msgs[BATCH_SIZE];

    for (size_t i = 0; i < BATCH_SIZE; ++i) {
        iovecs[i].iov_base = pkt_bufs[i];
        iovecs[i].iov_len  = MAX_PKT_BYTES;
        std::memset(&msgs[i], 0, sizeof(mmsghdr));
        msgs[i].msg_hdr.msg_iov    = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    timespec timeout{0, 100'000};  // 100µs timeout，确保 stop() 能及时生效

    while (running_.load(std::memory_order_relaxed)) {
        int nrecv = ::recvmmsg(sockfd_, msgs, BATCH_SIZE, MSG_DONTWAIT, &timeout);
        if (nrecv <= 0) continue;

        const uint64_t local_ts = rdtsc_ns();
        rx_pkts_.fetch_add(static_cast<uint64_t>(nrecv), std::memory_order_relaxed);

        for (int i = 0; i < nrecv; ++i) {
            process_packet(pkt_bufs[i], msgs[i].msg_len, local_ts);
        }
    }
#endif
}

// macOS / 通用 POSIX: recvmsg 逐包
void FeedHandler::recv_loop_posix() noexcept {
    alignas(64) uint8_t buf[MAX_PKT_BYTES];

    // 设置 socket 为非阻塞，通过 select 实现 100µs 轮询
    timeval tv{0, 100};  // 100µs
    ::setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (running_.load(std::memory_order_relaxed)) {
        ssize_t n = ::recv(sockfd_, buf, sizeof(buf), 0);
        if (n <= 0) continue;

        const uint64_t local_ts = rdtsc_ns();
        rx_pkts_.fetch_add(1, std::memory_order_relaxed);
        process_packet(buf, static_cast<size_t>(n), local_ts);
    }
}

}  // namespace hft
