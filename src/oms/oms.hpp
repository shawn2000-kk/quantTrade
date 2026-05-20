// src/oms/oms.hpp
// OMS 主逻辑：订单生命周期管理
//
// 职责：
//   - 分配 / 归还 Order 对象（MemoryPool，零 new/delete）
//   - 维护 client_order_id → Order* 映射（unordered_map O(1)）
//   - 接收 ACK / Fill / CancelAck 并驱动状态机
//   - 将成交事件转发给 FillTracker
//
// 线程安全：OMS 本身非线程安全，由 OMSThread 独占调用；
//           next_client_order_id_ 用 atomic 以便其他线程只读预分配。
#pragma once

#include <atomic>
#include <unordered_map>
#include <cstdint>

#include "oms/order.hpp"
#include "oms/order_state_machine.hpp"
#include "oms/fill_tracker.hpp"
#include "infra/memory_pool.hpp"
#include "common/types.hpp"

namespace hft {

class OMS {
public:
    explicit OMS(FillTracker& fill_tracker) noexcept;
    ~OMS() noexcept;

    OMS(const OMS&)            = delete;
    OMS& operator=(const OMS&) = delete;
    OMS(OMS&&)                 = delete;
    OMS& operator=(OMS&&)      = delete;

    // ── Strategy → OMS ──────────────────────────────────────────
    // 从内存池分配 Order，填充字段，状态 = PENDING_NEW，插入 map。
    // 返回 Order*（供 Gateway 层读取 client_order_id）；
    // 内存池耗尽时返回 nullptr。
    [[nodiscard]] Order* submit(const OrderRequest& req) noexcept;

    // ── Exchange → OMS ──────────────────────────────────────────
    // on_ack: 更新 exchange_order_id / ack_ts_ns，驱动状态机（NEW/REJECTED）
    bool on_ack(const OrderAck& ack) noexcept;

    // on_fill: 调用 OrderStateMachine::apply_fill，转发给 FillTracker
    bool on_fill(const FillEvent& fill) noexcept;

    // on_cancel_ack: 将订单状态转换为 CANCELLED
    bool on_cancel_ack(uint64_t client_order_id) noexcept;

    // ── 查找 & 释放 ────────────────────────────────────────────
    // find: O(1) 查找；未找到返回 nullptr
    [[nodiscard]] Order* find(uint64_t client_order_id) noexcept;

    // release: terminal 状态后从 map 移除并归还内存池
    //          非 terminal 状态也可调用（强制释放，调用方保证逻辑正确）
    void release(uint64_t client_order_id) noexcept;

    // ── 统计 ───────────────────────────────────────────────────
    [[nodiscard]] size_t active_orders() const noexcept {
        return orders_.size();
    }

private:
    FillTracker&                         fill_tracker_;
    MemoryPool<Order, 65536>             pool_;
    std::unordered_map<uint64_t, Order*> orders_;
    std::atomic<uint64_t>                next_order_id_{1};  // 单调递增
};

} // namespace hft
