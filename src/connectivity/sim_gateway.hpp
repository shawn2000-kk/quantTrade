#pragma once

#include <cstdint>
#include <queue>
#include <random>
#include <string_view>
#include <vector>

#include "connectivity/gateway.hpp"
#include "oms/order.hpp"

// ============================================================
// sim_gateway.hpp — 模拟网关（仅用于测试 / 回测，不含真实网络）
//
// 实现 IGateway，接受报单后：
//   1. 记录到 submitted_orders_（可供测试断言）
//   2. 按 fill_probability 概率生成 FillEvent 推入 fill_queue_
//      fill_probability=1.0：每笔必成交；0.0：永不成交
//
// is_connected() 默认返回 true，可通过 set_connected() 覆盖，
// 供 SessionManager 断连/重连场景的单元测试使用。
// ============================================================

namespace hft {

class SimGateway : public IGateway {
public:
    /// @param fill_probability  每笔报单被成交的概率 [0.0, 1.0]
    /// @param ack_latency_ns    get_rtt_ns() 返回的模拟往返延迟（纳秒）
    explicit SimGateway(double   fill_probability = 1.0,
                        uint64_t ack_latency_ns   = 1000) noexcept;

    // ── IGateway 实现 ────────────────────────────────────────────

    /// 记录报单，并根据 fill_probability 决定是否生成成交事件。
    void send_new_order(const OrderRequest& req) noexcept override;

    /// 空操作（SimGateway 不维护撤单逻辑）。
    void send_cancel(uint64_t client_order_id) noexcept override;

    /// 空操作（SimGateway 不维护改单逻辑）。
    void send_modify(const ModifyRequest& req) noexcept override;

    /// 返回当前连接状态（默认 true，可由 set_connected() 修改）。
    [[nodiscard]] bool is_connected() const noexcept override;

    /// 返回构造时传入的 ack_latency_ns_。
    [[nodiscard]] uint64_t get_rtt_ns() const noexcept override;

    [[nodiscard]] std::string_view name() const noexcept override;

    // ── 测试辅助接口 ─────────────────────────────────────────────

    /// 取出一个模拟成交事件。若队列为空返回 false，否则填充 out 并返回 true。
    bool pop_fill(FillEvent& out) noexcept;

    /// 已提交的报单总数。
    [[nodiscard]] size_t order_count() const noexcept;

    /// 强制设置连接状态，供 SessionManager 断连测试使用。
    void set_connected(bool v) noexcept;

private:
    double   fill_probability_;
    uint64_t ack_latency_ns_;
    bool     connected_{true};

    std::vector<OrderRequest> submitted_orders_;
    std::queue<FillEvent>     fill_queue_;

    /// 用于生成 exchange_order_id 的单调计数器
    uint64_t next_exchange_id_{1};

    /// 概率采样用 PRNG（Mersenne Twister + uniform [0,1) 分布）
    std::mt19937                          rng_;
    std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

} // namespace hft
