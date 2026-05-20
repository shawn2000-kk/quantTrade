#pragma once
#include "common/types.hpp"
#include "infra/spsc_queue.hpp"
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"

namespace hft {

// ── 策略基类（CRTP 静态分派，零虚函数调用开销）─────────────────────────
//
// 设计约束：
//   - 子类必须实现 on_bbo_impl / on_fill_impl / on_order_ack_impl
//   - 子类通过 send_order() 推入 order_queue_（策略线程）
//   - OMS 线程通过 pop_order() 取出待发报单
//   - pop_order / pop_cancel 在子类中声明为 public（using 提权）
//   - 整个基类无虚表，sizeof 只含 instrument_id_ + SPSC 队列

template<typename Derived>
class StrategyBase {
public:
    // ── CRTP 分派入口（由 StrategyManager 或上层调用）────────────
    void on_bbo(const BBOEvent& e) noexcept {
        static_cast<Derived*>(this)->on_bbo_impl(e);
    }

    void on_fill(const FillEvent& f) noexcept {
        static_cast<Derived*>(this)->on_fill_impl(f);
    }

    void on_order_ack(const OrderAck& a) noexcept {
        static_cast<Derived*>(this)->on_order_ack_impl(a);
    }

    // 禁止拷贝/移动（含 SPSC 队列，转移所有权无意义）
    StrategyBase(const StrategyBase&)            = delete;
    StrategyBase& operator=(const StrategyBase&) = delete;
    StrategyBase(StrategyBase&&)                 = delete;
    StrategyBase& operator=(StrategyBase&&)      = delete;

protected:
    StrategyBase() noexcept = default;
    ~StrategyBase() noexcept = default;

    // 策略线程调用：将报单请求推入 SPSC 队列（队列满返回 false）
    bool send_order(OrderRequest&& req) noexcept {
        return order_queue_.push(req);
    }

    // OMS 线程调用：取出下一条待发报单（队列空返回 false）
    bool pop_order(OrderRequest& out) noexcept {
        return order_queue_.pop(out);
    }

    // 策略关注的品种 ID（子类构造时写入，之后只读）
    InstrumentId instrument_id_{0};

private:
    // 策略 → OMS 报单通道（深度 1024，每个 slot 32 bytes，约 32KB）
    SPSCQueue<OrderRequest, 1024> order_queue_;
};

// ── 编译期验证：基类实例化不含虚表 ──────────────────────────────────────
// 此处用一个 "空" 派生类验证（仅在本编译单元内部）
namespace detail {

class StrategyBasePolymorphicCheck : public StrategyBase<StrategyBasePolymorphicCheck> {
public:
    void on_bbo_impl(const BBOEvent&) noexcept {}
    void on_fill_impl(const FillEvent&) noexcept {}
    void on_order_ack_impl(const OrderAck&) noexcept {}
};

static_assert(!std::is_polymorphic_v<StrategyBasePolymorphicCheck>,
    "StrategyBase must not introduce a vtable (no virtual functions)");

} // namespace detail

} // namespace hft
