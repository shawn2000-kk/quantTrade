// src/oms/order_state_machine.cpp
#include "oms/order_state_machine.hpp"

namespace hft {

// ── transition ────────────────────────────────────────────────────
//
// 只允许设计文档定义的合法边：
//   PENDING_NEW    → NEW | REJECTED
//   NEW            → PARTIALLY_FILLED | FILLED | PENDING_CANCEL
//   PARTIALLY_FILLED → PARTIALLY_FILLED | FILLED | PENDING_CANCEL
//   PENDING_CANCEL → CANCELLED | FILLED
//
// 所有其他转换均非法（包括从 terminal 状态再转换）。
bool OrderStateMachine::transition(Order& order,
                                   OrderStatus new_status) noexcept {
    bool valid = false;

    switch (order.status) {
        case OrderStatus::PENDING_NEW:
            valid = (new_status == OrderStatus::NEW ||
                     new_status == OrderStatus::REJECTED);
            break;

        case OrderStatus::NEW:
            valid = (new_status == OrderStatus::PARTIALLY_FILLED ||
                     new_status == OrderStatus::FILLED           ||
                     new_status == OrderStatus::PENDING_CANCEL);
            break;

        case OrderStatus::PARTIALLY_FILLED:
            valid = (new_status == OrderStatus::PARTIALLY_FILLED ||
                     new_status == OrderStatus::FILLED           ||
                     new_status == OrderStatus::PENDING_CANCEL);
            break;

        case OrderStatus::PENDING_CANCEL:
            valid = (new_status == OrderStatus::CANCELLED ||
                     new_status == OrderStatus::FILLED);
            break;

        // Terminal 状态不允许任何转换
        case OrderStatus::FILLED:
        case OrderStatus::CANCELLED:
        case OrderStatus::REJECTED:
        default:
            valid = false;
            break;
    }

    if (valid) {
        order.status = new_status;
        return true;
    }
    return false;
}

// ── apply_fill ────────────────────────────────────────────────────
//
// 1. 参数校验：fill_qty > 0 且订单处于活跃状态
// 2. 计算新 filled_qty，选取目标状态（PARTIALLY_FILLED / FILLED）
// 3. 调用 transition()；若状态机拒绝则回滚 filled_qty，返回 false
//    （例：PENDING_CANCEL 时部分成交是非法转换）
bool OrderStateMachine::apply_fill(Order& order,
                                   Qty fill_qty) noexcept {
    if (fill_qty <= 0)          return false;
    if (!order.is_active())     return false;

    const Qty old_filled = order.filled_qty;
    Qty new_filled = old_filled + fill_qty;

    // 防止 overfill：成交数量超出剩余时，钳位到 qty
    if (new_filled > order.qty) {
        new_filled = order.qty;
    }

    const OrderStatus target = (new_filled >= order.qty)
        ? OrderStatus::FILLED
        : OrderStatus::PARTIALLY_FILLED;

    // 先写 filled_qty，再让 transition 验证状态合法性
    order.filled_qty = new_filled;

    if (!transition(order, target)) {
        // 回滚
        order.filled_qty = old_filled;
        return false;
    }
    return true;
}

// ── status_name ───────────────────────────────────────────────────
const char* OrderStateMachine::status_name(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::PENDING_NEW:       return "PENDING_NEW";
        case OrderStatus::NEW:               return "NEW";
        case OrderStatus::PARTIALLY_FILLED:  return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:            return "FILLED";
        case OrderStatus::PENDING_CANCEL:    return "PENDING_CANCEL";
        case OrderStatus::CANCELLED:         return "CANCELLED";
        case OrderStatus::REJECTED:          return "REJECTED";
        default:                             return "UNKNOWN";
    }
}

} // namespace hft
