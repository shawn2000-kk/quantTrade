#pragma once
#include <atomic>
#include <cstdint>
#include "common/types.hpp"
#include "infra/spsc_queue.hpp"
#include "strategy/strategy_base.hpp"

namespace hft {

// ── 做市策略 ──────────────────────────────────────────────────────────
//
// 行为：
//   - 收到每次有效 BBO 后，若 mid 移动 >= 0.5 tick，撤旧单并按新 mid 报双边
//   - 报价：bid = mid - spread/2，ask = mid + spread/2（整数 tick 对齐）
//   - 库存偏斜：多头时 ask 降 1 tick（加速平多）；空头时 bid 升 1 tick（加速平空）
//   - 库存超限时暂停对应方向报单（多头超限停止买入，空头超限停止卖出）
//
// 线程安全：
//   - on_bbo_impl / on_order_ack_impl：策略线程调用
//   - on_fill_impl：OMS/持仓线程调用（inventory_ 为 atomic）
//   - pop_order / pop_cancel：OMS 线程调用

class MarketMaker : public StrategyBase<MarketMaker> {
public:
    // instrument_id : 策略关注的品种
    // spread_ticks  : 目标报价 spread（tick 数，建议为偶数以保持对称）
    // order_qty     : 每边报单数量（手）
    // max_inventory : 最大净持仓绝对值（超过后暂停对应方向报单）
    MarketMaker(InstrumentId instrument_id,
                Price        spread_ticks,
                Qty          order_qty,
                Qty          max_inventory) noexcept;

    // ── CRTP 回调（由 StrategyBase 分派）────────────────────────────
    void on_bbo_impl(const BBOEvent& e) noexcept;
    void on_fill_impl(const FillEvent& f) noexcept;
    void on_order_ack_impl(const OrderAck& a) noexcept;

    // ── OMS 线程接口 ─────────────────────────────────────────────────
    // 提权：StrategyBase 中为 protected，此处开放给 OMS 线程
    using StrategyBase<MarketMaker>::pop_order;

    // 取出待发撤单的 client_order_id（队列空返回 false）
    bool pop_cancel(uint64_t& out) noexcept {
        return cancel_queue_.pop(out);
    }

private:
    // ── 构造参数（不可变）────────────────────────────────────────────
    const Price spread_ticks_;
    const Qty   order_qty_;
    const Qty   max_inventory_;

    // ── 运行时状态 ───────────────────────────────────────────────────
    // 当前净持仓（正多负空）；on_fill_impl 从 OMS 线程写入
    std::atomic<Qty>   inventory_{0};

    // last_mid_ 存储最近一次报价时的 (bid_px + ask_px)（即 2x 实际 mid）
    // 变化量为 1 对应 mid 移动 0.5 tick，用于判断是否需要重新报价
    std::atomic<Price> last_mid_{0};

    // 当前挂单的 client_order_id（0 表示该方向无挂单）
    // 仅在策略线程访问（on_bbo_impl / on_order_ack_impl），无需 atomic
    uint64_t bid_order_id_{0};
    uint64_t ask_order_id_{0};

    // 待撤单队列：存放 client_order_id，由 OMS 线程消费
    SPSCQueue<uint64_t, 256> cancel_queue_;

    // 待 ACK 报单的方向队列：用于将 ACK 的 client_order_id 映射回 bid/ask
    // 按 FIFO 顺序：send bid 时 push BUY，send ask 时 push SELL
    SPSCQueue<Side, 64> pending_side_queue_;

    // ── 私有辅助 ─────────────────────────────────────────────────────
    void cancel_active_orders() noexcept;
    void quote(Price bid_price, Price ask_price, Qty inv) noexcept;
};

// 编译期验证：做市策略无虚表
static_assert(!std::is_polymorphic_v<MarketMaker>,
    "MarketMaker must not have a vtable (CRTP, no virtual functions)");

} // namespace hft
