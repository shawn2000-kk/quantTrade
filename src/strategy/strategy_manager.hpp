#pragma once
#include <functional>
#include <vector>
#include "market_data/market_data_types.hpp"

namespace hft {

// ── 策略生命周期管理器 ────────────────────────────────────────────────
//
// 负责将行情事件广播给所有已注册策略。
//
// 设计约束：
//   - 仅在非热路径（启动/配置阶段）注册策略
//   - dispatch_bbo 是唯一的热路径入口，遍历 handler vector 逐一回调
//   - 使用 std::function 封装类型擦除的回调，兼容任意 CRTP 策略类型
//   - 策略对象的生命周期必须长于 StrategyManager（回调捕获引用）

class StrategyManager {
public:
    StrategyManager() noexcept = default;
    ~StrategyManager() noexcept = default;

    // 不可拷贝（持有对外部策略对象的引用）
    StrategyManager(const StrategyManager&)            = delete;
    StrategyManager& operator=(const StrategyManager&) = delete;
    // 可移动（vector 可移动）
    StrategyManager(StrategyManager&&)            = default;
    StrategyManager& operator=(StrategyManager&&) = default;

    // ── 注册策略 ─────────────────────────────────────────────────────
    // 捕获 strategy 的引用；strategy 必须在 StrategyManager 销毁前有效
    template<typename S>
    void register_strategy(S& strategy) {
        bbo_handlers_.emplace_back([&strategy](const BBOEvent& e) noexcept {
            strategy.on_bbo(e);
        });
    }

    // ── BBO 事件广播 ─────────────────────────────────────────────────
    // 顺序调用所有注册策略的 on_bbo；无锁，不阻塞
    void dispatch_bbo(const BBOEvent& e) noexcept;

    // ── 已注册策略数量 ───────────────────────────────────────────────
    [[nodiscard]] size_t strategy_count() const noexcept {
        return bbo_handlers_.size();
    }

private:
    // 每个元素对应一个已注册策略的 on_bbo 回调（类型擦除）
    std::vector<std::function<void(const BBOEvent&)>> bbo_handlers_;
};

} // namespace hft
