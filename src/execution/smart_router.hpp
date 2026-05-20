// src/execution/smart_router.hpp
// 智能路由：从多个 Gateway 中选择 RTT 最小且已连接的一个发送报单
//
// 设计约束：
//   - 所有方法 noexcept，热路径不抛出
//   - 仅在 startup 时持有 Gateway 指针列表，运行期不做动态分配
//   - 全部断连时丢弃报单并计数，不阻塞调用线程

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

    // 不可拷贝 / 不可移动（内含 atomic dropped_ 计数器语义）
    SmartRouter(const SmartRouter&)            = delete;
    SmartRouter& operator=(const SmartRouter&) = delete;
    SmartRouter(SmartRouter&&)                 = delete;
    SmartRouter& operator=(SmartRouter&&)      = delete;

    // ── 核心接口 ─────────────────────────────────────────────────────

    /// 从已连接的 Gateway 中选 RTT 最小者并返回指针。
    /// 全部断连时返回 nullptr。
    [[nodiscard]] IGateway* select(const OrderRequest& req) noexcept;

    /// 调用 select()，成功则发单；失败则递增 dropped_ 计数。
    void send(const OrderRequest& req) noexcept;

    // ── 统计 ─────────────────────────────────────────────────────────

    /// 返回因全部断连而被丢弃的报单数。
    [[nodiscard]] uint64_t dropped_count() const noexcept;

private:
    std::vector<IGateway*> gateways_;
    uint64_t               dropped_{0};
};

} // namespace hft
