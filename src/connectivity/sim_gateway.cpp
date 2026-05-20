#include "connectivity/sim_gateway.hpp"

#include "infra/rdtsc_clock.hpp"  // rdtsc_ns()

namespace hft {

// ── 构造 ──────────────────────────────────────────────────────────

SimGateway::SimGateway(double fill_probability, uint64_t ack_latency_ns) noexcept
    : fill_probability_(fill_probability)
    , ack_latency_ns_(ack_latency_ns)
    , rng_(std::random_device{}())
{}

// ── IGateway 实现 ─────────────────────────────────────────────────

void SimGateway::send_new_order(const OrderRequest& req) noexcept {
    submitted_orders_.push_back(req);

    // 概率判断：是否生成成交事件
    //   fill_probability==1.0：dist_() 恒 < 1.0，必成交
    //   fill_probability==0.0：dist_() 恒 >= 0.0，永不成交（严格判断）
    const bool will_fill = (fill_probability_ >= 1.0)
                         || (fill_probability_ > 0.0 && dist_(rng_) < fill_probability_);

    if (will_fill) {
        FillEvent ev{};
        ev.client_order_id   = 0;                   // SimGateway 无 client_order_id
        ev.exchange_order_id = next_exchange_id_++;
        ev.instrument_id     = req.instrument_id;
        ev.side              = req.side;
        ev.fill_price        = req.price;
        ev.fill_qty          = req.qty;
        ev.remaining_qty     = 0;                   // 全部成交
        ev.fill_ts_ns        = rdtsc_ns();
        fill_queue_.push(ev);
    }
}

void SimGateway::send_cancel(uint64_t /*client_order_id*/) noexcept {
    // SimGateway 不维护撤单状态，空操作
}

void SimGateway::send_modify(const ModifyRequest& /*req*/) noexcept {
    // SimGateway 不维护改单状态，空操作
}

bool SimGateway::is_connected() const noexcept {
    return connected_;
}

uint64_t SimGateway::get_rtt_ns() const noexcept {
    return ack_latency_ns_;
}

std::string_view SimGateway::name() const noexcept {
    return "SimGateway";
}

// ── 测试辅助接口 ──────────────────────────────────────────────────

bool SimGateway::pop_fill(FillEvent& out) noexcept {
    if (fill_queue_.empty()) {
        return false;
    }
    out = fill_queue_.front();
    fill_queue_.pop();
    return true;
}

size_t SimGateway::order_count() const noexcept {
    return submitted_orders_.size();
}

void SimGateway::set_connected(bool v) noexcept {
    connected_ = v;
}

} // namespace hft
