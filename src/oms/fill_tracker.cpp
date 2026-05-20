// src/oms/fill_tracker.cpp
#include "oms/fill_tracker.hpp"

namespace hft {

void FillTracker::on_fill(const FillEvent& fill) noexcept {
    // 先更新原子统计（即使队列满也保证计数准确）
    fill_count_.fetch_add(1, std::memory_order_relaxed);

    if (fill.instrument_id < MAX_INSTRUMENTS) {
        instrument_qty_[fill.instrument_id].fetch_add(
            fill.fill_qty, std::memory_order_relaxed);
    }

    // 尽力推入队列；队列满时丢弃（热路径不阻塞）
    fill_queue_.push(fill);
}

bool FillTracker::pop_fill(FillEvent& out) noexcept {
    return fill_queue_.pop(out);
}

uint64_t FillTracker::total_fills() const noexcept {
    return fill_count_.load(std::memory_order_relaxed);
}

Qty FillTracker::total_filled_qty(InstrumentId id) const noexcept {
    if (id >= MAX_INSTRUMENTS) [[unlikely]] return 0;
    return instrument_qty_[id].load(std::memory_order_relaxed);
}

} // namespace hft
