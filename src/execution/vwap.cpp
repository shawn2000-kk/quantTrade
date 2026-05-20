// src/execution/vwap.cpp

#include "execution/vwap.hpp"

#include <algorithm>   // std::max, std::min
#include <cstdint>

namespace hft {

Vwap::Vwap(IGateway&     gw,
           InstrumentId  instrument_id,
           Side          side,
           Qty           total_qty,
           const double  volume_profile[],
           int           slices,
           uint64_t      duration_ns) noexcept
    : gw_           (gw)
    , instrument_id_(instrument_id)
    , side_         (side)
    , total_qty_    (total_qty)
    , slices_       (slices > 0 ? slices : 1)
    , duration_ns_  (duration_ns)
    , interval_ns_  (slices_ > 0
                         ? duration_ns_ / static_cast<uint64_t>(slices_)
                         : duration_ns_)
{
    const int n = slices_ < kVwapMaxSlices ? slices_ : kVwapMaxSlices;

    // 复制 volume_profile 并计算初始 target_qty
    Qty assigned = 0;
    for (int i = 0; i < n - 1; ++i) {
        volume_profile_[i] = (volume_profile != nullptr) ? volume_profile[i] : (1.0 / n);
        target_qty_[i]     = static_cast<Qty>(
                                 static_cast<double>(total_qty_) * volume_profile_[i]
                             );
        assigned += target_qty_[i];
    }
    // 最后一片取余量，确保总量精确
    volume_profile_[n - 1] = (volume_profile != nullptr) ? volume_profile[n - 1] : (1.0 / n);
    target_qty_[n - 1]     = total_qty_ - assigned;
}

void Vwap::start() noexcept {
    start_ns_       = rdtsc_ns();
    dispatched_qty_ = 0;
    total_filled_   = 0;
    slice_index_    = 0;
}

void Vwap::on_fill(Qty fill_qty) noexcept {
    total_filled_ += fill_qty;
}

void Vwap::maybe_rebalance() noexcept {
    // 只在已完成至少一片、且已有成交回报时才做偏差检测
    // （尚无成交时无法评估偏差，不触发重平衡）
    if (slice_index_ == 0) return;
    if (total_filled_ == 0) return;

    // 计算前 slice_index_ 片的原始目标量（用原始 volume_profile_ 比例）
    double orig_frac = 0.0;
    for (int i = 0; i < slice_index_; ++i) {
        orig_frac += volume_profile_[i];
    }
    const Qty expected = static_cast<Qty>(static_cast<double>(total_qty_) * orig_frac);
    if (expected <= 0) return;

    const double deviation =
        static_cast<double>(total_filled_ - expected) / static_cast<double>(expected);

    if (deviation > 0.20 || deviation < -0.20) {
        // 重新分配剩余量到后续片
        const Qty remaining_to_fill = total_qty_ - total_filled_;
        if (remaining_to_fill <= 0) return;

        const int rem_slices = slices_ - slice_index_;
        if (rem_slices <= 0) return;

        // 后续切片的 profile 权重之和
        double rem_profile_sum = 0.0;
        for (int i = slice_index_; i < slices_; ++i) {
            rem_profile_sum += volume_profile_[i];
        }
        if (rem_profile_sum <= 0.0) return;

        Qty assigned = 0;
        for (int i = slice_index_; i < slices_ - 1; ++i) {
            target_qty_[i] = static_cast<Qty>(
                static_cast<double>(remaining_to_fill)
                * volume_profile_[i] / rem_profile_sum
            );
            assigned += target_qty_[i];
        }
        // 最后一片吸收余量
        target_qty_[slices_ - 1] = remaining_to_fill - assigned;
    }
}

bool Vwap::tick() noexcept {
    if (slice_index_ >= slices_) return false;

    const uint64_t now          = rdtsc_ns();
    const uint64_t threshold_ns = start_ns_
                                  + static_cast<uint64_t>(slice_index_) * interval_ns_;
    if (now < threshold_ns) return false;

    // 偏差检测与重平衡（仅在前几片已完成后）
    maybe_rebalance();

    // 计算本片发单量
    Qty qty;
    if (slice_index_ == slices_ - 1) {
        // 最后一片：发出所有剩余量
        qty = total_qty_ - dispatched_qty_;
    } else {
        qty = target_qty_[slice_index_];
        // 不超过剩余总量
        const Qty available = total_qty_ - dispatched_qty_
                              - static_cast<Qty>(slices_ - 1 - slice_index_);
        qty = std::min(qty, available);
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
        dispatched_qty_ += qty;
    }

    ++slice_index_;
    return true;
}

bool Vwap::is_done() const noexcept {
    return slice_index_ >= slices_;
}

} // namespace hft
