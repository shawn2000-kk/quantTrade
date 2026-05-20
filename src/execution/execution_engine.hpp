// src/execution/execution_engine.hpp
// 执行引擎主控
//
// 统一入口：持有 SmartRouter，提供 submit() 将报单路由到最优 Gateway。
// 后续可在此层加入节流、批量聚合等逻辑，策略层保持对 ExecutionEngine 的单一依赖。

#pragma once

#include <vector>

#include "execution/smart_router.hpp"
#include "connectivity/gateway.hpp"
#include "oms/order.hpp"

namespace hft {

class ExecutionEngine {
public:
    /// 以一组 Gateway 指针初始化（顺序即优先级候选池）。
    explicit ExecutionEngine(std::vector<IGateway*> gateways) noexcept;

    // 不可拷贝 / 不可移动
    ExecutionEngine(const ExecutionEngine&)            = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;
    ExecutionEngine(ExecutionEngine&&)                 = delete;
    ExecutionEngine& operator=(ExecutionEngine&&)      = delete;

    // ── 热路径 ───────────────────────────────────────────────────────

    /// 将报单提交给 SmartRouter，由其选择最优 Gateway 发出。
    void submit(const OrderRequest& req) noexcept;

    // ── 访问器 ───────────────────────────────────────────────────────

    [[nodiscard]] SmartRouter& router() noexcept;

private:
    SmartRouter router_;
};

} // namespace hft
