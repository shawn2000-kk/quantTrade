#pragma once
#include <cstddef>
#include "common/types.hpp"
#include "position/position_manager.hpp"

namespace hft {

// PnlCalculator — MTM（逐日盯市）盈亏计算
//
// 依赖 PositionManager 提供实时持仓与均价，计算：
//   - unrealized_pnl：持仓按当前 mark_price 估算的浮动盈亏
//   - total_pnl      ：realized_pnl + unrealized_pnl
//
// 公式（对多空均适用）：
//   unrealized = (mark_price - avg_cost) * net_qty
//   - 多头（net_qty > 0）：mark > avg_cost → 盈利（正值）✓
//   - 空头（net_qty < 0）：mark < avg_cost → 盈利（正值）
//     例：avg_cost=120, mark=110, net_qty=-100
//         (110-120) * (-100) = -10 * -100 = 1000 > 0 ✓
//
// 所有方法 noexcept，兼容 -fno-exceptions -fno-rtti。

class PnlCalculator {
public:
    // 构造时绑定 PositionManager（生命周期必须长于 PnlCalculator）
    explicit PnlCalculator(const PositionManager& pm) noexcept : pm_(pm) {}

    // 禁止拷贝（持有引用成员）
    PnlCalculator(const PnlCalculator&)            = delete;
    PnlCalculator& operator=(const PnlCalculator&) = delete;

    // ── 单品种 unrealized PnL ─────────────────────────────────────
    // 返回 (mark_price - avg_cost) * net_qty（tick * qty 单位）
    // id 越界或仓位为 0 时返回 0
    Price calc_unrealized_pnl(InstrumentId id, Price mark_price) const noexcept;

    // ── 多品种汇总 unrealized PnL ─────────────────────────────────
    // 遍历前 n 个品种（n <= MAX_INSTRUMENTS），用 mark_prices[i] 计算并求和
    // mark_prices 为长度至少为 n 的数组
    Price calc_total_unrealized_pnl(const Price mark_prices[], size_t n) const noexcept;

    // ── 总 PnL = realized + unrealized ───────────────────────────
    Price calc_total_pnl(const Price mark_prices[], size_t n) const noexcept;

private:
    const PositionManager& pm_;
};

} // namespace hft
