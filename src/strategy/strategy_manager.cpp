#include "strategy/strategy_manager.hpp"

namespace hft {

void StrategyManager::dispatch_bbo(const BBOEvent& e) noexcept {
    for (auto& handler : bbo_handlers_) {
        handler(e);
    }
}

} // namespace hft
