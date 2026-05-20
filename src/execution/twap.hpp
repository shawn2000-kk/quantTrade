// src/execution/twap.hpp
// TWAP（时间加权平均价）执行算法
//
// 将大单均匀切分到 N 个等时间窗口；每片数量加 ±jitter_ratio 随机抖动，
// 防止被算法猎手识别规律性报单。
//
// 时钟：使用 rdtsc_ns()（热路径），不使用 std::chrono。
// 线程：单线程调用 start() / tick()，不做内部同步。

#pragma once

#include <cstdint>

#include "connectivity/gateway.hpp"
#include "common/types.hpp"
#include "oms/order.hpp"
#include "infra/rdtsc_clock.hpp"

namespace hft {

class Twap {
public:
    /// @param gw           目标 Gateway（生命周期由调用方管理）
    /// @param instrument_id 品种 ID
    /// @param side          买卖方向
    /// @param total_qty     总量（手）
    /// @param duration_ns   执行总时长（纳秒）
    /// @param slices        切分片数（>= 1）
    /// @param jitter_ratio  每片数量的随机抖动比例 ±（默认 0.1 = ±10%）
    Twap(IGateway&    gw,
         InstrumentId instrument_id,
         Side         side,
         Qty          total_qty,
         uint64_t     duration_ns,
         int          slices,
         double       jitter_ratio = 0.1) noexcept;

    // 不可拷贝 / 不可移动（引用成员）
    Twap(const Twap&)            = delete;
    Twap& operator=(const Twap&) = delete;
    Twap(Twap&&)                 = delete;
    Twap& operator=(Twap&&)      = delete;

    // ── 生命周期 ─────────────────────────────────────────────────────

    /// 记录 start_ns_，重置内部状态。调用后可立即开始 tick()。
    void start() noexcept;

    // ── 热路径：每次 spin-loop 或定时回调中调用 ──────────────────────

    /// 检查是否到达下一片的发单时间点：
    ///   - 到达则发一笔报单（含 jitter），返回 true
    ///   - 尚未到达或已全部发完，返回 false
    bool tick() noexcept;

    // ── 状态查询 ─────────────────────────────────────────────────────

    /// 所有片均已发出时返回 true
    [[nodiscard]] bool is_done() const noexcept;

    /// 尚未发出的剩余数量
    [[nodiscard]] Qty remaining_qty() const noexcept;

private:
    /// LCG 随机数生成（避免使用 std::mt19937 的动态初始化）
    Qty apply_jitter(Qty base_qty) noexcept;

    IGateway&    gw_;
    InstrumentId instrument_id_;
    Side         side_;
    Qty          total_qty_;
    uint64_t     duration_ns_;
    int          slices_;
    double       jitter_ratio_;

    Qty      slice_qty_;     ///< 基础切片大小 = total_qty_ / slices_
    Qty      sent_qty_{0};   ///< 已提交到 Gateway 的累计数量
    int      slice_index_{0};///< 下一个待发片的索引
    uint64_t start_ns_{0};   ///< start() 时记录的 RDTSC 纳秒
    uint64_t interval_ns_;   ///< 相邻两片之间的时间间隔 = duration_ns_ / slices_
    uint64_t lcg_state_;     ///< LCG 随机数状态
};

} // namespace hft
