// src/execution/vwap.hpp
// VWAP（成交量加权平均价）执行算法
//
// 按历史成交量占比分配各时间片的目标量；若前几片实际成交量与目标偏差 > 20%，
// 则按原始 volume_profile 比例重新分配后续片的量（实时偏差修正）。
//
// 接口风格与 Twap 保持一致（start / tick / is_done）。
// 时钟：使用 rdtsc_ns()，不使用 std::chrono。

#pragma once

#include <cstdint>

#include "connectivity/gateway.hpp"
#include "common/types.hpp"
#include "oms/order.hpp"
#include "infra/rdtsc_clock.hpp"

namespace hft {

/// 最大支持的切片数（固定大小数组，避免动态分配）
inline constexpr int kVwapMaxSlices = 128;

class Vwap {
public:
    /// @param gw              目标 Gateway
    /// @param instrument_id   品种 ID
    /// @param side            买卖方向
    /// @param total_qty       总量（手）
    /// @param volume_profile  长度为 slices 的历史成交量占比数组（各元素之和 = 1.0）
    /// @param slices          切分片数（1 ~ kVwapMaxSlices）
    /// @param duration_ns     执行总时长（纳秒）
    Vwap(IGateway&     gw,
         InstrumentId  instrument_id,
         Side          side,
         Qty           total_qty,
         const double  volume_profile[],
         int           slices,
         uint64_t      duration_ns) noexcept;

    // 不可拷贝 / 不可移动（引用成员）
    Vwap(const Vwap&)            = delete;
    Vwap& operator=(const Vwap&) = delete;
    Vwap(Vwap&&)                 = delete;
    Vwap& operator=(Vwap&&)      = delete;

    // ── 生命周期 ─────────────────────────────────────────────────────

    /// 记录 start_ns_，重置内部状态。
    void start() noexcept;

    // ── 热路径 ───────────────────────────────────────────────────────

    /// 检查是否到达下一片的发单时间点：
    ///   - 到达则（按需重新平衡后）发一笔报单，返回 true
    ///   - 尚未到达或已全部发完，返回 false
    bool tick() noexcept;

    // ── 成交回调 ─────────────────────────────────────────────────────

    /// OMS / Gateway 回报成交时调用；更新已成交量，驱动偏差修正逻辑。
    void on_fill(Qty fill_qty) noexcept;

    // ── 状态查询 ─────────────────────────────────────────────────────

    [[nodiscard]] bool is_done() const noexcept;

private:
    /// 若前几片实际成交量与原始目标偏差 > 20%，重新分配后续片的量。
    void maybe_rebalance() noexcept;

    IGateway&    gw_;
    InstrumentId instrument_id_;
    Side         side_;
    Qty          total_qty_;
    int          slices_;
    uint64_t     duration_ns_;

    /// 原始 volume_profile（不变，用于偏差计算与重平衡时的权重分配）
    double volume_profile_[kVwapMaxSlices];

    /// 当前各片目标量（可在运行期被 maybe_rebalance 修改）
    Qty    target_qty_[kVwapMaxSlices];

    Qty      dispatched_qty_{0};  ///< 已提交到 Gateway 的累计量
    Qty      total_filled_{0};    ///< 通过 on_fill 累积的成交量
    int      slice_index_{0};     ///< 下一个待发片的索引
    uint64_t start_ns_{0};
    uint64_t interval_ns_;        ///< = duration_ns_ / slices_
};

} // namespace hft
