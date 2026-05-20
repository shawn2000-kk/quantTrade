#include <type_traits>
#include "market_data/market_data_types.hpp"
#include "oms/order.hpp"

// 所有检查在编译期完成，无需运行时代码

namespace hft {

// ── common/types.hpp ─────────────────────────────────────────
static_assert(sizeof(Price)        == 8);
static_assert(sizeof(Qty)          == 8);
static_assert(sizeof(InstrumentId) == 4);
static_assert(sizeof(Timestamp)    == 8);

// ── market_data_types.hpp ─────────────────────────────────────
static_assert(sizeof(PriceLevel)  == 16);
static_assert(sizeof(BBOEvent)    == 64,  "BBOEvent: must be exactly 1 cacheline");
static_assert(alignof(BBOEvent)   == 64,  "BBOEvent: must be cacheline-aligned");
static_assert(std::is_trivially_copyable_v<PriceLevel>, "PriceLevel: SPSC requires trivial copy");
static_assert(std::is_trivially_copyable_v<BBOEvent>,   "BBOEvent: SPSC requires trivial copy");
static_assert(std::is_trivially_copyable_v<DepthEvent>, "DepthEvent: requires trivial copy");

// ── order.hpp ─────────────────────────────────────────────────
static_assert(sizeof(OrderRequest) == 32, "OrderRequest: hot-path SPSC element size");
static_assert(sizeof(Order)        == 64, "Order: must fit in exactly 1 cacheline");
static_assert(alignof(Order)       == 64, "Order: must be cacheline-aligned");
static_assert(sizeof(FillEvent)    == 64, "FillEvent: must fit in exactly 1 cacheline");
static_assert(alignof(FillEvent)   == 64, "FillEvent: must be cacheline-aligned");
static_assert(std::is_trivially_copyable_v<OrderRequest>, "OrderRequest: SPSC requires trivial copy");
static_assert(std::is_trivially_copyable_v<Order>,        "Order: MemoryPool requires trivial copy");
static_assert(std::is_trivially_copyable_v<FillEvent>,    "FillEvent: SPSC requires trivial copy");
static_assert(std::is_trivially_copyable_v<OrderAck>,     "OrderAck: requires trivial copy");
static_assert(std::is_trivially_copyable_v<ModifyRequest>);

// 无循环依赖验证：common/types.hpp 只依赖 <cstdint>，以上头文件均可独立 include
// （编译通过即验证成功）

} // namespace hft

int main() { return 0; }
