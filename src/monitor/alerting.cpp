// ============================================================
// alerting.cpp
// ============================================================

#include "monitor/alerting.hpp"
#include "infra/rdtsc_clock.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// Linux-only: POSIX socket for HTTP POST
#ifdef __linux__
#   include <arpa/inet.h>
#   include <netdb.h>
#   include <sys/socket.h>
#   include <sys/time.h>
#   include <unistd.h>
#endif

namespace hft {

// ------------------------------------------------------------
// Internal helpers
// ------------------------------------------------------------
namespace {

static const char* level_str(AlertLevel lv) noexcept {
    switch (lv) {
        case AlertLevel::INFO:     return "INFO";
        case AlertLevel::WARN:     return "WARN";
        case AlertLevel::CRITICAL: return "CRITICAL";
        default:                   return "UNKNOWN";
    }
}

/// 将字符串值 JSON 转义后追加到 out
static void json_append_escaped(std::string& out, std::string_view s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // 控制字符 → \u00XX
                    char buf[8];
                    ::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
}

/// 构建 JSON 告警体
static std::string build_json(AlertLevel level,
                               std::string_view name,
                               std::string_view message,
                               uint64_t ts_ns) {
    std::string j;
    j.reserve(256);
    j += "{\"level\":\"";
    j += level_str(level);
    j += "\",\"name\":\"";
    json_append_escaped(j, name);
    j += "\",\"message\":\"";
    json_append_escaped(j, message);
    j += "\",\"ts_ns\":";
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ts_ns);
    j.append(buf, ptr);
    j += '}';
    return j;
}

// --------------------------------------------------------
// Linux 实现：阻塞 HTTP POST（在 detach 线程中调用）
// --------------------------------------------------------
#ifdef __linux__

// URL 解析：http://host[:port]/path（仅 Linux 路径使用）
struct ParsedUrl {
    std::string host;
    std::string port;  // string，传给 getaddrinfo
    std::string path;
};

static ParsedUrl parse_url(const std::string& url) noexcept {
    ParsedUrl result;
    result.port = "80";
    result.path = "/";

    std::string_view sv = url;

    // 跳过协议头
    if (sv.starts_with("http://"))       sv.remove_prefix(7);
    else if (sv.starts_with("https://")) sv.remove_prefix(8);

    // 找到第一个 '/'：分隔 host:port 与 path
    auto slash = sv.find('/');
    std::string_view hostport;
    if (slash == std::string_view::npos) {
        hostport = sv;
    } else {
        hostport = sv.substr(0, slash);
        result.path = std::string(sv.substr(slash));
    }

    // 分离 host 和 port
    auto colon = hostport.rfind(':');
    if (colon == std::string_view::npos) {
        result.host = std::string(hostport);
    } else {
        result.host = std::string(hostport.substr(0, colon));
        result.port = std::string(hostport.substr(colon + 1));
    }

    return result;
}

static void do_http_post(const std::string& webhook_url,
                         const std::string& json_body) noexcept {
    const ParsedUrl pu = parse_url(webhook_url);
    if (pu.host.empty()) return;

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    if (::getaddrinfo(pu.host.c_str(), pu.port.c_str(), &hints, &res) != 0) return;

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return;
    }

    // 5 秒超时（send/recv）
    struct timeval tv{ .tv_sec = 5, .tv_usec = 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        // 构建 HTTP 请求
        std::string req;
        req.reserve(512 + json_body.size());
        req += "POST ";
        req += pu.path;
        req += " HTTP/1.0\r\n";
        req += "Host: ";
        req += pu.host;
        req += "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: ";
        char buf[16];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf),
                                       static_cast<uint64_t>(json_body.size()));
        req.append(buf, ptr);
        req += "\r\n";
        req += "Connection: close\r\n";
        req += "\r\n";
        req += json_body;

        // 完整发送（忽略返回值，监控路径不阻塞主线程）
        size_t sent = 0;
        while (sent < req.size()) {
            const ssize_t n = ::send(fd, req.data() + sent,
                                     req.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    ::close(fd);
    ::freeaddrinfo(res);
}

#else // macOS / other — HTTP 发送为 no-op

static void do_http_post(const std::string& /*webhook_url*/,
                         const std::string& /*json_body*/) noexcept {
    // 非 Linux 平台不发送 HTTP
}

#endif // __linux__

} // anonymous namespace

// ------------------------------------------------------------
// Alerting
// ------------------------------------------------------------

Alerting::Alerting(std::string webhook_url) noexcept
    : webhook_url_(std::move(webhook_url))
{
    history_.reserve(kMaxHistory);
}

void Alerting::fire(AlertLevel level,
                    std::string_view name,
                    std::string_view message) noexcept {
    const uint64_t ts = rdtsc_ns();

    // 打印到 stderr（同步，快速）
    ::fprintf(stderr, "[%s] %.*s: %.*s\n",
              level_str(level),
              static_cast<int>(name.size()),    name.data(),
              static_cast<int>(message.size()), message.data());

    // 写入历史（加锁，最多 kMaxHistory 条，超出则移除最老的）
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (history_.size() >= kMaxHistory) {
            history_.erase(history_.begin());
        }
        history_.push_back(Alert{level, std::string(name), std::string(message), ts});
    }

    // 异步 HTTP POST（webhook_url_ 非空时）
    if (!webhook_url_.empty()) {
        send_async(build_json(level, name, message, ts));
    }
}

void Alerting::send_async(std::string json_body) noexcept {
    std::string url = webhook_url_;  // capture by value
    std::thread t([url = std::move(url), body = std::move(json_body)]() {
        do_http_post(url, body);
    });
    t.detach();
}

// --------------------------------------------------------
// 预定义业务告警
// --------------------------------------------------------

void Alerting::circuit_breaker_open(std::string_view exchange) noexcept {
    std::string msg = "Circuit breaker OPEN on exchange: ";
    msg += exchange;
    fire(AlertLevel::CRITICAL, "circuit_breaker_open", msg);
}

void Alerting::connection_lost(std::string_view exchange,
                               uint64_t         duration_ms) noexcept {
    std::string msg = "Connection lost to ";
    msg += exchange;
    msg += " for ";
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), duration_ms);
    msg.append(buf, ptr);
    msg += " ms";
    fire(AlertLevel::WARN, "connection_lost", msg);
}

void Alerting::latency_spike(std::string_view stage,
                              uint64_t         p99_ns,
                              uint64_t         threshold_ns) noexcept {
    std::string msg = "Latency spike on stage '";
    msg += stage;
    msg += "': p99=";
    {
        char buf[24];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), p99_ns);
        msg.append(buf, ptr);
    }
    msg += "ns, threshold=";
    {
        char buf[24];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), threshold_ns);
        msg.append(buf, ptr);
    }
    msg += "ns";
    fire(AlertLevel::WARN, "latency_spike", msg);
}

// --------------------------------------------------------
// count()
// --------------------------------------------------------

size_t Alerting::count(AlertLevel level) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& a : history_) {
        if (a.level == level) ++n;
    }
    return n;
}

} // namespace hft
