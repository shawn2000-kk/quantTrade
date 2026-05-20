#include "market_data/order_book.hpp"

namespace hft {

// ── SpinGuard ─────────────────────────────────────────────────────

OrderBook::SpinGuard::SpinGuard(std::atomic_flag& f) noexcept : flag_(f) {
    // test_and_set(acquire) 返回 true 表示已被其他线程持有，继续自旋
    while (flag_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        // PAUSE 指令：降低超线程竞争功耗，减少 memory order violation 流水线冲刷
        __builtin_ia32_pause();
#endif
    }
}

OrderBook::SpinGuard::~SpinGuard() noexcept {
    flag_.clear(std::memory_order_release);
}

// ── OrderBook ─────────────────────────────────────────────────────

OrderBook::OrderBook(InstrumentId id) noexcept
    : instrument_id_(id)
{}

// ── apply_bbo ─────────────────────────────────────────────────────
// 将 BBOEvent 的 bid/ask 写入第 0 档（最优档）
// 若品种首次收到 BBO，同时将有效档数设为 1
void OrderBook::apply_bbo(const BBOEvent& e) noexcept {
    SpinGuard g(lock_);
    bids_[0] = {e.bid_px, e.bid_qty};
    asks_[0] = {e.ask_px, e.ask_qty};
    if (bid_count_ == 0) bid_count_ = 1;
    if (ask_count_ == 0) ask_count_ = 1;
}

// ── apply_depth ───────────────────────────────────────────────────
// 全量替换 bid/ask 档位；超出 MAX_LEVELS 的部分截断
void OrderBook::apply_depth(const DepthEvent& e) noexcept {
    const uint8_t nb = (e.bid_levels <= MAX_LEVELS) ? e.bid_levels : MAX_LEVELS;
    const uint8_t na = (e.ask_levels <= MAX_LEVELS) ? e.ask_levels : MAX_LEVELS;
    SpinGuard g(lock_);
    for (uint8_t i = 0; i < nb; ++i) bids_[i] = e.bids[i];
    for (uint8_t i = 0; i < na; ++i) asks_[i] = e.asks[i];
    bid_count_ = nb;
    ask_count_ = na;
}

// ── snapshot_bbo ──────────────────────────────────────────────────
BBOEvent OrderBook::snapshot_bbo() const noexcept {
    SpinGuard g(lock_);
    BBOEvent ev{};
    ev.instrument_id = instrument_id_;
    ev.type          = EventType::BBO_UPDATE;
    if (bid_count_ > 0) {
        ev.bid_px  = bids_[0].price;
        ev.bid_qty = bids_[0].qty;
    }
    if (ask_count_ > 0) {
        ev.ask_px  = asks_[0].price;
        ev.ask_qty = asks_[0].qty;
    }
    return ev;
}

// ── snapshot_depth ────────────────────────────────────────────────
// 返回最多 MARKET_DEPTH(5) 档，实际档数由 bid_count_ / ask_count_ 决定
DepthEvent OrderBook::snapshot_depth() const noexcept {
    constexpr uint8_t DEPTH = static_cast<uint8_t>(MARKET_DEPTH);
    SpinGuard g(lock_);
    DepthEvent d{};
    d.instrument_id = instrument_id_;
    d.bid_levels    = (bid_count_ < DEPTH) ? bid_count_ : DEPTH;
    d.ask_levels    = (ask_count_ < DEPTH) ? ask_count_ : DEPTH;
    for (uint8_t i = 0; i < d.bid_levels; ++i) d.bids[i] = bids_[i];
    for (uint8_t i = 0; i < d.ask_levels; ++i) d.asks[i] = asks_[i];
    return d;
}

// ── mid_price ─────────────────────────────────────────────────────
Price OrderBook::mid_price() const noexcept {
    SpinGuard g(lock_);
    if (bid_count_ > 0 && ask_count_ > 0 &&
        bids_[0].price > 0 && asks_[0].price > 0) {
        return (bids_[0].price + asks_[0].price) / 2;
    }
    return 0;
}

// ── spread ────────────────────────────────────────────────────────
Price OrderBook::spread() const noexcept {
    SpinGuard g(lock_);
    if (bid_count_ > 0 && ask_count_ > 0 &&
        bids_[0].price > 0 && asks_[0].price > 0) {
        return asks_[0].price - bids_[0].price;
    }
    return 0;
}

// ── is_crossed ────────────────────────────────────────────────────
// bid[0].price >= ask[0].price → 穿越行情（异常）
bool OrderBook::is_crossed() const noexcept {
    SpinGuard g(lock_);
    if (bid_count_ > 0 && ask_count_ > 0) {
        return bids_[0].price >= asks_[0].price;
    }
    return false;
}

InstrumentId OrderBook::instrument_id() const noexcept {
    return instrument_id_;
}

}  // namespace hft
