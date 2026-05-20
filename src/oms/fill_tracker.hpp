// src/oms/fill_tracker.hpp
// 成交跟踪：将成交事件推入 SPSC 队列供 PositionThread 消费，
// 并维护全局及品种级别的累计成交统计（原子计数，无锁）。
//
// 线程模型：
//   生产者（OMSThread）：on_fill()
//   消费者（PositionThread）：pop_fill()
//   统计读取可来自任意线程（relaxed 原子读）
#pragma once

#include <atomic>
#include <cstdint>

#include "oms/order.hpp"
#include "infra/spsc_queue.hpp"
#include "common/types.hpp"

namespace hft {

class FillTracker {
public:
    FillTracker()  noexcept = default;
    ~FillTracker() noexcept = default;

    // 不可拷贝 / 移动（含 atomic 和 SPSCQueue 成员）
    FillTracker(const FillTracker&)            = delete;
    FillTracker& operator=(const FillTracker&) = delete;
    FillTracker(FillTracker&&)                 = delete;
    FillTracker& operator=(FillTracker&&)      = delete;

    // ── 生产者（OMSThread）────────────────────────────────────────
    // 推入 fill_queue_；更新全局 / 品种级原子计数。
    // 若队列已满，丢弃入队（统计仍更新），不阻塞，不返回错误。
    void on_fill(const FillEvent& fill) noexcept;

    // ── 消费者（PositionThread）──────────────────────────────────
    // 取出队头成交；队列空返回 false，out 不改变。
    bool pop_fill(FillEvent& out) noexcept;

    // ── 统计（任意线程，relaxed 读）──────────────────────────────
    uint64_t total_fills()                         const noexcept;
    Qty      total_filled_qty(InstrumentId id)     const noexcept;

private:
    // SPSC 成交队列（1024 槽，2 的幂）
    SPSCQueue<FillEvent, 1024> fill_queue_;

    // 全局成交笔数
    alignas(64) std::atomic<uint64_t> fill_count_{0};

    // 品种级别累计成交量（MAX_INSTRUMENTS = 4096，每个 8 字节）
    // 注：相邻品种共享 cacheline；热品种多时可考虑 padded array，
    // 当前以内存紧凑优先。
    std::atomic<Qty> instrument_qty_[MAX_INSTRUMENTS]{};
};

} // namespace hft
