// src/execution/smart_router.hpp
// 智能路由：基于流动性评分（深度 × 速度）从多 Gateway 中选路
//
// 评分公式：score = depth_proxy / rtt_ns
//   - depth_proxy：由外部通过 update_depth() 注入（来自 OrderBook 快照）
//   - rtt_ns      ：由 gateway.get_rtt_ns() 获取
//
// 选路策略：
//   1. 跳过未连接的 Gateway
//   2. 计算每个 Gateway 的流动性评分
//   3. 选评分最高者；若无可用 Gateway 则丢弃报单
//
// 设计约束：
//   - 所有方法 noexcept，热路径不抛出
//   - 仅在 startup 时持有 Gateway 指针列表，运行期不做动态分配
//   - depth_proxy 数组按 gateways_ 下标对应，初始值 1（避免除零）

#pragma once

#include <cstdint>
#include <vector>

#include "connectivity/gateway.hpp"
#include "oms/order.hpp"

namespace hft {

class SmartRouter {
public:
    /// 构造时传入所有可用 Gateway（生命周期由调用方管理）
    explicit SmartRouter(std::vector<IGateway*> gateways) noexcept;

    SmartRouter(const SmartRouter&)            = delete;
    SmartRouter& operator=(const SmartRouter&) = delete;
    SmartRouter(SmartRouter&&)                 = delete;
    SmartRouter& operator=(SmartRouter&&)      = delete;

    // ── 流动性更新（非热路径，由行情线程或监控线程定期调用）───────────
    // gateway_idx : gateways_ 中的下标
    // depth       : 当前可用深度（bid+ask 各一档量之和，或加权多档）
    void update_depth(size_t gateway_idx, int64_t depth) noexcept;

    // ── 核心接口 ─────────────────────────────────────────────────────

    /// 选择流动性评分最高的已连接 Gateway，全部断连时返回 nullptr。
    [[nodiscard]] IGateway* select(const OrderRequest& req) noexcept;

    /// 调用 select()，成功则发单；失败则递增 dropped_ 计数。
    void send(const OrderRequest& req) noexcept;

    // ── 统计 ─────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t dropped_count() const noexcept;

private:
    std::vector<IGateway*> gateways_;

    // depth_proxy_[i] 对应 gateways_[i] 的深度估计，初始为 1
    std::vector<int64_t>   depth_proxy_;

    uint64_t dropped_{0};
};

} // namespace hft
