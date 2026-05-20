// src/oms/order_state_machine.hpp
// 订单状态机：无状态，纯静态方法，验证并执行状态转换
//
// 合法转换图：
//   PENDING_NEW → NEW | REJECTED
//   NEW → PARTIALLY_FILLED | FILLED | PENDING_CANCEL
//   PARTIALLY_FILLED → PARTIALLY_FILLED | FILLED | PENDING_CANCEL
//   PENDING_CANCEL → CANCELLED | FILLED
#pragma once

#include "oms/order.hpp"

namespace hft {

class OrderStateMachine {
public:
    // 不可实例化：纯静态工具类
    OrderStateMachine()  = delete;
    ~OrderStateMachine() = delete;

    // transition: 验证 cur→new_status 是否合法
    //   合法：更新 order.status，返回 true
    //   非法：order 不变，返回 false
    static bool transition(Order& order, OrderStatus new_status) noexcept;

    // apply_fill: 累加 fill_qty，自动选取 PARTIALLY_FILLED / FILLED
    //   失败条件：fill_qty <= 0，order 非活跃，或状态机拒绝转换
    static bool apply_fill(Order& order, Qty fill_qty) noexcept;

    // status_name: 返回调试用枚举字符串（永不返回 nullptr）
    static const char* status_name(OrderStatus status) noexcept;
};

} // namespace hft
