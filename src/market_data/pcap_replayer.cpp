#include "market_data/pcap_replayer.hpp"
#include "infra/rdtsc_clock.hpp"
#include "infra/logger.hpp"

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

namespace hft {

// PCAP magic numbers
static constexpr uint32_t PCAP_MAGIC_LE   = 0xa1b2c3d4u;  // LE, microsecond
static constexpr uint32_t PCAP_MAGIC_NS   = 0xa1b23c4du;  // LE, nanosecond
static constexpr uint32_t PCAP_MAGIC_BE_LE = 0xd4c3b2a1u; // BE, microsecond

// Ethernet + IPv4 + UDP header size (minimal, no options)
static constexpr size_t L2L3L4_OFFSET = 14 + 20 + 8;  // 42 bytes

static uint32_t bswap32(uint32_t v) noexcept {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

PcapReplayer::PcapReplayer(const std::string& pcap_path,
                           ExchangeType       exchange,
                           Mode               mode) noexcept
    : pcap_path_(pcap_path)
    , exchange_(exchange)
    , mode_(mode)
{}

PcapReplayer::~PcapReplayer() noexcept {
    close_file();
}

bool PcapReplayer::open_file() noexcept {
    fp_ = std::fopen(pcap_path_.c_str(), "rb");
    if (!fp_) {
        HFT_LOG_ERROR("pcap_replayer", "cannot open: {}", pcap_path_);
        return false;
    }

    PcapGlobalHeader gh{};
    if (std::fread(&gh, 1, sizeof(gh), static_cast<FILE*>(fp_)) != sizeof(gh)) {
        HFT_LOG_ERROR("pcap_replayer", "truncated pcap header: {}", pcap_path_);
        return false;
    }

    if (gh.magic_number == PCAP_MAGIC_LE) {
        nano_ts_      = false;
        swap_endian_  = false;
    } else if (gh.magic_number == PCAP_MAGIC_NS) {
        nano_ts_      = true;
        swap_endian_  = false;
    } else if (gh.magic_number == PCAP_MAGIC_BE_LE) {
        nano_ts_      = false;
        swap_endian_  = true;
    } else {
        HFT_LOG_ERROR("pcap_replayer", "unknown pcap magic: {:08x}", gh.magic_number);
        return false;
    }

    if (swap_endian_) {
        gh.snaplen = bswap32(gh.snaplen);
        gh.network = bswap32(gh.network);
    }

    HFT_LOG_INFO("pcap_replayer", "opened {} link={} ns_ts={}",
                 pcap_path_, gh.network, nano_ts_);
    return true;
}

void PcapReplayer::close_file() noexcept {
    if (fp_) {
        std::fclose(static_cast<FILE*>(fp_));
        fp_ = nullptr;
    }
}

void PcapReplayer::dispatch_payload(const uint8_t* payload, size_t len,
                                    uint64_t pkt_ts_ns) noexcept {
    if (len < 1) return;

    const uint8_t msg_type = payload[4];  // WireBBO/WireDepth msg_type offset

    if (msg_type == WIRE_BBO || msg_type == WIRE_TRADE) {
        if (auto e = Normalizer::parse_bbo(payload, len, exchange_)) {
            e->local_ts_ns = pkt_ts_ns;
            ++msgs_replayed_;
            if (bbo_cb_) bbo_cb_(*e);
        } else {
            ++parse_errors_;
        }
    } else if (msg_type == WIRE_DEPTH) {
        if (auto e = Normalizer::parse_depth(payload, len, exchange_)) {
            e->ts_ns = pkt_ts_ns;
            ++msgs_replayed_;
            if (depth_cb_) depth_cb_(*e);
        } else {
            ++parse_errors_;
        }
    }
}

bool PcapReplayer::replay() noexcept {
    if (!open_file()) return false;

    running_.store(true, std::memory_order_relaxed);

    // 第一个包的时间戳（用于 REALTIME 模式的参考基准）
    uint64_t first_pkt_ts_ns  = 0;
    uint64_t first_wall_ns    = rdtsc_ns();
    bool     first_packet     = true;

    alignas(64) uint8_t pkt_buf[65536];

    while (running_.load(std::memory_order_relaxed)) {
        PcapPktHeader ph{};
        if (std::fread(&ph, 1, sizeof(ph), static_cast<FILE*>(fp_)) != sizeof(ph)) {
            break;  // EOF
        }
        if (swap_endian_) {
            ph.ts_sec   = bswap32(ph.ts_sec);
            ph.ts_usec  = bswap32(ph.ts_usec);
            ph.incl_len = bswap32(ph.incl_len);
            ph.orig_len = bswap32(ph.orig_len);
        }

        const uint32_t read_len = ph.incl_len;
        if (read_len > sizeof(pkt_buf)) {
            // 包太大，跳过
            std::fseek(static_cast<FILE*>(fp_), static_cast<long>(read_len), SEEK_CUR);
            continue;
        }

        if (std::fread(pkt_buf, 1, read_len, static_cast<FILE*>(fp_)) != read_len) {
            break;
        }

        // 计算包的时间戳（纳秒）
        uint64_t pkt_ts_ns;
        if (nano_ts_) {
            pkt_ts_ns = static_cast<uint64_t>(ph.ts_sec) * 1'000'000'000ULL
                      + ph.ts_usec;
        } else {
            pkt_ts_ns = static_cast<uint64_t>(ph.ts_sec) * 1'000'000'000ULL
                      + static_cast<uint64_t>(ph.ts_usec) * 1'000ULL;
        }

        if (first_packet) {
            first_pkt_ts_ns = pkt_ts_ns;
            first_wall_ns   = rdtsc_ns();
            first_packet    = false;
        }

        // REALTIME 模式：按原始时间间隔回放
        if (mode_ == Mode::REALTIME) {
            const uint64_t delta_pcap = pkt_ts_ns - first_pkt_ts_ns;
            const uint64_t wall_elapsed = rdtsc_ns() - first_wall_ns;
            if (delta_pcap > wall_elapsed) {
                const uint64_t sleep_ns = delta_pcap - wall_elapsed;
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
            }
        }

        ++pkts_replayed_;

        // 跳过以太网 + IP + UDP 头，提取应用层 payload
        if (read_len <= L2L3L4_OFFSET) continue;

        const uint8_t* payload     = pkt_buf + L2L3L4_OFFSET;
        const size_t   payload_len = read_len - L2L3L4_OFFSET;
        dispatch_payload(payload, payload_len, pkt_ts_ns);
    }

    close_file();
    running_.store(false, std::memory_order_relaxed);
    HFT_LOG_INFO("pcap_replayer", "done pkts={} msgs={} errors={}",
                 pkts_replayed_, msgs_replayed_, parse_errors_);
    return true;
}

}  // namespace hft
