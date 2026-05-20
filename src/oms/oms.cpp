// src/oms/oms.cpp
#include "oms/oms.hpp"
#include "infra/rdtsc_clock.hpp"

#include <new>  // placement new

namespace hft {

OMS::OMS(FillTracker& fill_tracker) noexcept
    : fill_tracker_(fill_tracker)
{
    // 预留 map 桶，减少热路径 rehash
    orders_.reserve(4096);
}

OMS::~OMS() noexcept {
    // 归还所有残留订单到内存池（正常运行时应已通过 release() 清零）
    for (auto& [id, order] : orders_) {
        order->~Order();
        pool_.release(order);
    }
    orders_.clear();
}

// ── submit ────────────────────────────────────────────────────────
// 从内存池取一块原始内存，placement new 初始化 Order，插入 map。
Order* OMS::submit(const OrderRequest& req) noexcept {
    void* raw = pool_.acquire();
    if (!raw) [[unlikely]] return nullptr;

    // placement new：Order 有 trivial 析构，但构造必须调用以设置默认成员
    Order* order = new (raw) Order{};

    order->client_order_id  = next_order_id_.fetch_add(1, std::memory_order_relaxed);
    order->exchange_order_id = 0;
    order->instrument_id    = req.instrument_id;
    order->side             = req.side;
    order->type             = req.type;
    order->status           = OrderStatus::PENDING_NEW;
    order->price            = req.price;
    order->qty              = req.qty;
    order->filled_qty       = 0;
    order->submit_ts_ns     = rdtsc_ns();
    order->ack_ts_ns        = 0;

    orders_.emplace(order->client_order_id, order);
    return order;
}

// ── on_ack ────────────────────────────────────────────────────────
// 处理交易所 ACK：更新 exchange_order_id / ack_ts_ns，驱动状态机。
// ack.status 只应是 NEW 或 REJECTED（调用方保证）。
bool OMS::on_ack(const OrderAck& ack) noexcept {
    Order* order = find(ack.client_order_id);
    if (!order) [[unlikely]] return false;

    order->exchange_order_id = ack.exchange_order_id;
    order->ack_ts_ns         = ack.ack_ts_ns;

    return OrderStateMachine::transition(*order, ack.status);
}

// ── on_fill ───────────────────────────────────────────────────────
// 处理成交回报：驱动状态机累积成交量，转发给 FillTracker。
bool OMS::on_fill(const FillEvent& fill) noexcept {
    Order* order = find(fill.client_order_id);
    if (!order) [[unlikely]] return false;

    const bool ok = OrderStateMachine::apply_fill(*order, fill.fill_qty);
    if (ok) {
        fill_tracker_.on_fill(fill);
    }
    return ok;
}

// ── on_cancel_ack ─────────────────────────────────────────────────
bool OMS::on_cancel_ack(uint64_t client_order_id) noexcept {
    Order* order = find(client_order_id);
    if (!order) [[unlikely]] return false;

    return OrderStateMachine::transition(*order, OrderStatus::CANCELLED);
}

// ── find ──────────────────────────────────────────────────────────
Order* OMS::find(uint64_t client_order_id) noexcept {
    const auto it = orders_.find(client_order_id);
    return (it != orders_.end()) ? it->second : nullptr;
}

// ── release ───────────────────────────────────────────────────────
// 从 map 移除后归还内存池。Order 为 trivially destructible，
// 调用 ~Order() 是 no-op，但保持语义完整性。
void OMS::release(uint64_t client_order_id) noexcept {
    const auto it = orders_.find(client_order_id);
    if (it == orders_.end()) [[unlikely]] return;

    Order* order = it->second;
    orders_.erase(it);

    order->~Order();
    pool_.release(order);
}

} // namespace hft
