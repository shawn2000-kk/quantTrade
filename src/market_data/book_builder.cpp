#include "market_data/book_builder.hpp"

namespace hft {

// ── get_or_create ─────────────────────────────────────────────────
// 先在 map 中查找；未找到则 emplace 一个新 OrderBook
// noexcept：内存分配失败→ std::terminate，符合 HFT 系统策略
OrderBook& BookBuilder::get_or_create(InstrumentId id) noexcept {
    auto it = books_.find(id);
    if (it != books_.end()) {
        return *it->second;
    }
    // 品种首次出现，懒初始化
    auto [ins_it, ok] = books_.emplace(id, std::make_unique<OrderBook>(id));
    (void)ok;
    return *ins_it->second;
}

// ── on_bbo ────────────────────────────────────────────────────────
void BookBuilder::on_bbo(const BBOEvent& e) noexcept {
    get_or_create(e.instrument_id).apply_bbo(e);
}

// ── on_depth ──────────────────────────────────────────────────────
void BookBuilder::on_depth(const DepthEvent& e) noexcept {
    get_or_create(e.instrument_id).apply_depth(e);
}

// ── find (const) ─────────────────────────────────────────────────
const OrderBook* BookBuilder::find(InstrumentId id) const noexcept {
    auto it = books_.find(id);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

// ── find (non-const) ─────────────────────────────────────────────
OrderBook* BookBuilder::find(InstrumentId id) noexcept {
    auto it = books_.find(id);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

// ── book_count ────────────────────────────────────────────────────
size_t BookBuilder::book_count() const noexcept {
    return books_.size();
}

}  // namespace hft
