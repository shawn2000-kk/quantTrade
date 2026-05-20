// tests/unit/test_memory_pool.cpp
// GoogleTest 单元测试：MemoryPool<T, N>
//
// 编译示例（手动，无 CMake）：
//   c++ -std=c++20 -I src \
//       tests/unit/test_memory_pool.cpp \
//       -lgtest -lgtest_main -o test_memory_pool && ./test_memory_pool

#include <gtest/gtest.h>
#include <cstdint>
#include <set>
#include <unordered_set>

#include "infra/memory_pool.hpp"
#include "oms/order.hpp"  // Order (64 bytes, alignas(64))

namespace hft {

// ── 辅助：检查指针是否属于此 pool 的 storage 范围 ─────────────────
template<typename T, size_t N>
bool ptr_in_pool(const MemoryPool<T, N>& pool, const T* ptr) {
    // MemoryPool 不暴露 storage_，通过 acquire/release 全部用完再还回来
    // 这里通过指针地址检查替代方案：见下方单独测试
    (void)pool; (void)ptr;
    return true;  // placeholder，实际通过行为测试
}

// ── 初始状态 ────────────────────────────────────────────────────────
TEST(MemoryPoolTest, InitialState) {
    MemoryPool<Order, 8> pool;
    EXPECT_EQ(pool.available(), 8u);
    EXPECT_EQ(pool.capacity(),  8u);
    EXPECT_TRUE(pool.empty());   // empty() = 所有 slot 空闲
    EXPECT_FALSE(pool.full());
}

// ── acquire 基本正确性 ───────────────────────────────────────────────
TEST(MemoryPoolTest, AcquireReturnsNonNull) {
    MemoryPool<Order, 4> pool;

    Order* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 3u);
    EXPECT_FALSE(pool.empty());
    EXPECT_FALSE(pool.full());
}

// ── release 基本正确性 ───────────────────────────────────────────────
TEST(MemoryPoolTest, ReleaseRestoresAvailability) {
    MemoryPool<Order, 4> pool;

    Order* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 3u);

    pool.release(p);
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_TRUE(pool.empty());  // 全部归还 = empty
}

// ── 池满时 acquire 返回 nullptr ─────────────────────────────────────
TEST(MemoryPoolTest, AcquireReturnsNullptrWhenFull) {
    constexpr size_t N = 4;
    MemoryPool<Order, N> pool;

    Order* ptrs[N];
    for (size_t i = 0; i < N; ++i) {
        ptrs[i] = pool.acquire();
        ASSERT_NE(ptrs[i], nullptr) << "acquire() failed at i=" << i;
    }
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_TRUE(pool.full());

    // 第 N+1 次 acquire 应返回 nullptr
    Order* extra = pool.acquire();
    EXPECT_EQ(extra, nullptr);
    EXPECT_EQ(pool.available(), 0u);  // 不变

    // 清理
    for (size_t i = 0; i < N; ++i) pool.release(ptrs[i]);
}

// ── release 后可重新 acquire ─────────────────────────────────────────
TEST(MemoryPoolTest, AcquireAfterRelease) {
    MemoryPool<Order, 2> pool;

    Order* p1 = pool.acquire();
    Order* p2 = pool.acquire();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // 归还 p1
    pool.release(p1);
    EXPECT_EQ(pool.available(), 1u);

    // 重新获取，应该能成功
    Order* p3 = pool.acquire();
    ASSERT_NE(p3, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // 清理
    pool.release(p2);
    pool.release(p3);
    EXPECT_EQ(pool.available(), 2u);
}

// ── available() 计数正确：全部 acquire 再逐个 release ────────────────
TEST(MemoryPoolTest, AvailableCountTracksCorrectly) {
    constexpr size_t N = 8;
    MemoryPool<Order, N> pool;

    Order* ptrs[N];
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(pool.available(), N - i);
        ptrs[i] = pool.acquire();
        ASSERT_NE(ptrs[i], nullptr);
    }
    EXPECT_EQ(pool.available(), 0u);

    for (size_t i = 0; i < N; ++i) {
        pool.release(ptrs[i]);
        EXPECT_EQ(pool.available(), i + 1);
    }
    EXPECT_EQ(pool.available(), N);
}

// ── 所有 slot 地址互不相同 ──────────────────────────────────────────
TEST(MemoryPoolTest, AllSlotsAreDistinct) {
    constexpr size_t N = 16;
    MemoryPool<Order, N> pool;

    std::unordered_set<Order*> ptrs;
    for (size_t i = 0; i < N; ++i) {
        Order* p = pool.acquire();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(ptrs.insert(p).second)  // 插入成功 = 新地址
            << "Duplicate slot address returned at i=" << i;
    }
    EXPECT_EQ(ptrs.size(), N);

    // 清理
    for (Order* p : ptrs) pool.release(p);
}

// ── 所有 slot 满足 T 的对齐要求 ────────────────────────────────────
TEST(MemoryPoolTest, SlotsAreAligned) {
    constexpr size_t N = 8;
    MemoryPool<Order, N> pool;  // Order: alignas(64)

    for (size_t i = 0; i < N; ++i) {
        Order* p = pool.acquire();
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(Order), 0u)
            << "Slot " << i << " is not properly aligned";
    }
    // 全部 acquire 完毕，先不 release，pool.full() 应为 true
    EXPECT_TRUE(pool.full());

    // 注意：这里 pool 被析构时 N 个 slot 都是"已 acquire"状态
    // MemoryPool 不管理对象生命周期，析构时直接释放 storage_（std::byte[]），无 UB
}

// ── 以 Order 结构体做写入验证（placement new 用法示例） ──────────────
TEST(MemoryPoolTest, PlacementNewAndManualDtor) {
    MemoryPool<Order, 4> pool;

    // 获取原始内存后执行 placement new
    Order* p = pool.acquire();
    ASSERT_NE(p, nullptr);

    new (p) Order{};          // 构造
    p->client_order_id  = 0xDEADBEEFULL;
    p->instrument_id    = 42;
    p->price            = 99800;
    p->qty              = 100;
    p->status           = OrderStatus::NEW;

    EXPECT_EQ(p->client_order_id, 0xDEADBEEFULL);
    EXPECT_EQ(p->instrument_id, 42u);
    EXPECT_EQ(p->price, 99800);
    EXPECT_EQ(p->qty, 100);

    p->~Order();           // 手动析构
    pool.release(p);       // 归还原始内存
    EXPECT_EQ(pool.available(), 4u);
}

// ── available() 在多次 acquire/release 交替后保持一致 ────────────────
TEST(MemoryPoolTest, InterleavedAcquireRelease) {
    MemoryPool<Order, 4> pool;

    Order* a = pool.acquire();   EXPECT_EQ(pool.available(), 3u);
    Order* b = pool.acquire();   EXPECT_EQ(pool.available(), 2u);
    pool.release(a);             EXPECT_EQ(pool.available(), 3u);
    Order* c = pool.acquire();   EXPECT_EQ(pool.available(), 2u);
    pool.release(b);             EXPECT_EQ(pool.available(), 3u);
    pool.release(c);             EXPECT_EQ(pool.available(), 4u);

    EXPECT_TRUE(pool.empty());
}

// ── 小类型（8 字节，恰好等于 sizeof(void*)）─────────────────────────
// 验证 sizeof(T) == sizeof(void*) 时 free-list 刚好可以存放 next 指针
struct alignas(8) Slot8 {
    uint64_t value;
};
static_assert(sizeof(Slot8) == sizeof(void*));
static_assert(std::is_trivially_copyable_v<Slot8>);

TEST(MemoryPoolTest, SmallTypeExactPointerSize) {
    MemoryPool<Slot8, 8> pool;
    EXPECT_EQ(pool.available(), 8u);

    Slot8* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    p->value = 0xCAFEBABEULL;
    EXPECT_EQ(p->value, 0xCAFEBABEULL);

    pool.release(p);
    EXPECT_EQ(pool.available(), 8u);
}

}  // namespace hft
