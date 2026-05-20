#pragma once
#include <memory>
#include <unordered_map>
#include "common/types.hpp"
#include "market_data/market_data_types.hpp"
#include "market_data/order_book.hpp"

namespace hft {

// ── BookBuilder：多品种订单簿管理 ────────────────────────────────────
//
// 设计约束：
//   - BookBuilder 不在热路径上（管理路径），允许使用 std::unordered_map
//   - 懒初始化：首次收到某品种事件时创建对应 OrderBook
//   - 最多支持 MAX_INSTRUMENTS(4096) 个品种（由调用方保证，此处不强制截断）
//   - get_or_create / on_bbo / on_depth 均标注 noexcept；
//     std::unordered_map::operator[] / unique_ptr 分配失败在 noexcept 下
//     会调用 std::terminate，符合 HFT 系统"宁终止不返回错误状态"原则
//   - find() 提供 const/非-const 两个重载，方便策略层只读访问

class BookBuilder {
public:
    BookBuilder()  = default;
    ~BookBuilder() = default;

    // 不可拷贝 / 移动（unique_ptr 语义，避免意外复制）
    BookBuilder(const BookBuilder&)            = delete;
    BookBuilder& operator=(const BookBuilder&) = delete;
    BookBuilder(BookBuilder&&)                 = delete;
    BookBuilder& operator=(BookBuilder&&)      = delete;

    // ── 管理接口 ────────────────────────────────────────────────────

    // 懒初始化：若品种不存在则创建 OrderBook，返回引用
    OrderBook& get_or_create(InstrumentId id) noexcept;

    // 路由：将 BBOEvent 派发到对应品种的 OrderBook（自动创建若不存在）
    void on_bbo(const BBOEvent& e) noexcept;

    // 路由：将 DepthEvent 派发到对应品种的 OrderBook（自动创建若不存在）
    void on_depth(const DepthEvent& e) noexcept;

    // ── 查询接口 ────────────────────────────────────────────────────

    // 按品种 ID 查找：找不到返回 nullptr
    const OrderBook* find(InstrumentId id) const noexcept;
    OrderBook*       find(InstrumentId id) noexcept;

    // 当前已创建的 OrderBook 数量
    [[nodiscard]] size_t book_count() const noexcept;

private:
    std::unordered_map<InstrumentId, std::unique_ptr<OrderBook>> books_;
};

}  // namespace hft
