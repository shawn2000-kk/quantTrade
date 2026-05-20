// src/execution/twap.cpp

#include "execution/twap.hpp"

#include <algorithm>   // std::max, std::min
#include <cstdint>

namespace hft {

// ────────────────────────────────────────────────────────────────────
// LCG 常数（Knuth / Newlib 参数；64-bit multiply-add）
// ────────────────────────────────────────────────────────────────────
static constexpr uint64_t kLcgA = 6364136223846793005ULL;
static constexpr uint64_t kLcgC = 1442695040888963407ULL;

Twap::Twap(IGateway&    gw,
           InstrumentId instrument_id,
           Side         side,
           Qty          total_qty,
           uint64_t     duration_ns,
           int          slices,
           double       jitter_ratio) noexcept
    : gw_           (gw)
    , instrument_id_(instrument_id)
    , side_         (side)
    , total_qty_    (total_qty)
    , duration_ns_  (duration_ns)
    , slices_       (slices > 0 ? slices : 1)
    , jitter_ratio_ (jitter_ratio)
    , slice_qty_    (total_qty_ / static_cast<Qty>(slices_))
    , interval_ns_  (slices_ > 0
                         ? duration_ns_ / static_cast<uint64_t>(slices_)
                         : duration_ns_)
    , lcg_state_    (0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(total_qty))
{}

void Twap::start() noexcept {
    start_ns_    = rdtsc_ns();
    sent_qty_    = 0;
    slice_index_ = 0;
}

Qty Twap::apply_jitter(Qty base_qty) noexcept {
    // Advance LCG state
    lcg_state_ = lcg_state_ * kLcgA + kLcgC;

    // Map high 32 bits to [0, 2^32 - 1], then to [-1.0, +1.0]
    const double r      = static_cast<double>(lcg_state_ >> 32)
                          / static_cast<double>(0xFFFFFFFFULL);   // [0.0, 1.0]
    const double jitter = (r * 2.0 - 1.0) * jitter_ratio_;         // [-ratio, +ratio]
    const Qty    result = static_cast<Qty>(
                              static_cast<double>(base_qty) * (1.0 + jitter)
                          );
    return std::max(Qty{1}, result);
}

bool Twap::tick() noexcept {
    if (slice_index_ >= slices_) return false;

    const uint64_t now          = rdtsc_ns();
    const uint64_t threshold_ns = start_ns_
                                  + static_cast<uint64_t>(slice_index_) * interval_ns_;

    if (now < threshold_ns) return false;

    // Determine quantity for this slice
    Qty qty;
    if (slice_index_ == slices_ - 1) {
        // Last slice: send exact remainder to ensure total_qty_ is fully executed
        qty = total_qty_ - sent_qty_;
    } else {
        qty = apply_jitter(slice_qty_);
        // Guard: never overshoot total (preserve headroom for remaining slices)
        const Qty headroom = total_qty_ - sent_qty_
                             - static_cast<Qty>(slices_ - 1 - slice_index_);
        qty = std::min(qty, headroom);
        qty = std::max(Qty{1}, qty);
    }

    if (qty > 0) {
        OrderRequest req{};
        req.instrument_id  = instrument_id_;
        req.side           = side_;
        req.type           = OrderType::MARKET;
        req.price          = 0;
        req.qty            = qty;
        req.strategy_ts_ns = rdtsc_ns();
        gw_.send_new_order(req);
        sent_qty_ += qty;
    }

    ++slice_index_;
    return true;
}

bool Twap::is_done() const noexcept {
    return slice_index_ >= slices_;
}

Qty Twap::remaining_qty() const noexcept {
    return total_qty_ - sent_qty_;
}

} // namespace hft
