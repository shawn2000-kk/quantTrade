// src/execution/smart_router.cpp

#include "execution/smart_router.hpp"
#include <cstdint>
#include <limits>

namespace hft {

SmartRouter::SmartRouter(std::vector<IGateway*> gateways) noexcept
    : gateways_(std::move(gateways))
    , depth_proxy_(gateways_.size(), 1)
    , dropped_{0}
{}

void SmartRouter::update_depth(size_t gateway_idx, int64_t depth) noexcept {
    if (gateway_idx < depth_proxy_.size()) {
        depth_proxy_[gateway_idx] = (depth > 0) ? depth : 1;
    }
}

// ── 流动性评分选路 ────────────────────────────────────────────────────────
//
// 每个 Gateway 的评分：
//   score = depth_proxy / max(rtt_ns, 1)
//
// 分子（depth_proxy）越大 → 流动性越好，越倾向选这条路
// 分母（rtt_ns）越小     → 延迟越低，越倾向选这条路
//
// RTT = 0（未测量）时视为 1ns，避免除零，同时给予最优先（新连接）

IGateway* SmartRouter::select(const OrderRequest& /*req*/) noexcept {
    IGateway* best      = nullptr;
    double    best_score = -1.0;

    for (size_t i = 0; i < gateways_.size(); ++i) {
        IGateway* gw = gateways_[i];
        if (!gw || !gw->is_connected()) continue;

        const uint64_t rtt   = gw->get_rtt_ns();
        const double   denom = (rtt > 0) ? static_cast<double>(rtt) : 1.0;
        const double   score = static_cast<double>(depth_proxy_[i]) / denom;

        if (score > best_score) {
            best_score = score;
            best       = gw;
        }
    }
    return best;
}

void SmartRouter::send(const OrderRequest& req) noexcept {
    IGateway* gw = select(req);
    if (gw) {
        gw->send_new_order(req);
    } else {
        ++dropped_;
    }
}

uint64_t SmartRouter::dropped_count() const noexcept {
    return dropped_;
}

} // namespace hft
