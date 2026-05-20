// src/execution/pov.hpp
// POV（参与率，Percentage of Volume）执行算法
//
// 跟踪市场实时成交量，按固定参与率（如 5%）动态调整子单量：
//   target_qty = market_volume_ × participation_rate
//   若 target_qty - sent_qty_ > min_order_size，发一笔报单
//
// 不依赖时钟：完全由市场成交事件驱动（on_market_trade 回调）。

#pragma once

#include <cstdint>

#include "connectivity/gateway.hpp"
#include "common/types.hpp"
#include "oms/order.hpp"

namespace hft {

class Pov {
public:
    /// @param gw                 目标 Gateway
    /// @param instrument_id      品种 ID
    /// @param side               买卖方向
    /// @param total_qty          总量上限（达到后停止发单）
    /// @param participation_rate 参与率（0.0 ~ 1.0，如 0.05 = 5%）
    Pov(IGateway&    gw,
        InstrumentId instrument_id,
        Side         side,
        Qty          total_qty,
        double       participation_rate) noexcept;

    // 不可拷贝 / 不可移动
    Pov(const Pov&)            = delete;
    Pov& operator=(const Pov&) = delete;
    Pov(Pov&&)                 = delete;
    Pov& operator=(Pov&&)      = delete;

    // ── 市场事件回调 ─────────────────────────────────────────────────

    /// 每次市场成交时调用（逐笔或批量均可）：
    ///   累加 market_volume_；若参与目标与已成交之差 > min_order_size，发一笔报单。
    void on_market_trade(Qty market_trade_qty) noexcept;

    // ── 成交回调 ─────────────────────────────────────────────────────

    /// OMS / Gateway 回报成交时调用；更新 sent_qty_（已成交量）。
    void on_fill(Qty fill_qty) noexcept;

    // ── 状态查询 ─────────────────────────────────────────────────────

    /// sent_qty_ >= total_qty_ 时返回 true（已完成全部目标量）。
    [[nodiscard]] bool is_done() const noexcept;

private:
    static constexpr Qty kMinOrderSize = 1;  ///< 最小发单量阈值

    IGateway&    gw_;
    InstrumentId instrument_id_;
    Side         side_;
    Qty          total_qty_;
    double       participation_rate_;

    Qty    market_volume_{0};  ///< 累积市场成交量
    Qty    sent_qty_{0};       ///< 已成交量（通过 on_fill 更新）
    Qty    ordered_qty_{0};    ///< 已提交到 Gateway 的累计量（防重复发单）
};

} // namespace hft
