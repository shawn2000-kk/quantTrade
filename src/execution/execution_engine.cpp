// src/execution/execution_engine.cpp

#include "execution/execution_engine.hpp"

namespace hft {

ExecutionEngine::ExecutionEngine(std::vector<IGateway*> gateways) noexcept
    : router_(std::move(gateways))
{}

void ExecutionEngine::submit(const OrderRequest& req) noexcept {
    router_.send(req);
}

SmartRouter& ExecutionEngine::router() noexcept {
    return router_;
}

} // namespace hft
