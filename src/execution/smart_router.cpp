// src/execution/smart_router.cpp

#include "execution/smart_router.hpp"

#include <cstdint>

namespace hft {

SmartRouter::SmartRouter(std::vector<IGateway*> gateways) noexcept
    : gateways_(std::move(gateways))
    , dropped_{0}
{}

IGateway* SmartRouter::select(const OrderRequest& /*req*/) noexcept {
    IGateway* best    = nullptr;
    uint64_t  min_rtt = UINT64_MAX;

    for (IGateway* gw : gateways_) {
        if (!gw || !gw->is_connected()) continue;

        const uint64_t rtt = gw->get_rtt_ns();
        if (rtt < min_rtt) {
            min_rtt = rtt;
            best    = gw;
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
