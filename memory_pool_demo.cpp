/**
 * @file memory_pool_demo.cpp
 * @brief Unit-style checks and a simple benchmark vs heap allocation.
 *
 * Build (example):
 *   g++ -std=c++17 -O2 -pthread memory_pool.cpp memory_pool_demo.cpp -o memory_pool_demo
 *   cl /EHsc /std:c++17 /O2 memory_pool.cpp memory_pool_demo.cpp
 */

#include "memory_pool.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>

namespace {

void assert_true(const char* name, bool ok) {
    if (!ok) {
        std::cerr << "FAIL: " << name << '\n';
        std::exit(1);
    }
    std::cout << "ok: " << name << '\n';
}

void test_basic_round_trip() {
    MemoryPool pool(32, 10, alignof(std::max_align_t), false);
    assert_true("capacity", pool.capacity() == 10);
    assert_true("initial free", pool.free_count() == 10);

    void* a = pool.allocate();
    void* b = pool.allocate();
    assert_true("non-null alloc", a && b && a != b);
    assert_true("free after 2 alloc", pool.free_count() == 8);

    assert_true("dealloc a", pool.deallocate(a) == MemoryPool::DeallocateResult::Success);
    assert_true("dealloc b", pool.deallocate(b) == MemoryPool::DeallocateResult::Success);
    assert_true("free restored", pool.free_count() == 10);
}

void test_exhaustion_returns_null() {
    MemoryPool pool(16, 3, 64, false);
    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    void* p3 = pool.allocate();
    void* p4 = pool.allocate();
    assert_true("three ok", p1 && p2 && p3);
    assert_true("fourth null", p4 == nullptr);
    pool.deallocate(p1);
    pool.deallocate(p2);
    pool.deallocate(p3);
}

void test_invalid_pointer() {
    MemoryPool pool(24, 4, 32, false);
    int stack_x = 0;
    assert_true("stack ptr rejected",
                pool.deallocate(&stack_x) == MemoryPool::DeallocateResult::InvalidPointer);

    void* p = pool.allocate();
    std::byte* mis = reinterpret_cast<std::byte*>(p) + 4;
    assert_true("misaligned rejected",
                pool.deallocate(mis) == MemoryPool::DeallocateResult::InvalidPointer);
    pool.deallocate(p);
}

void test_double_free() {
    MemoryPool pool(8, 2, 16, false);
    void* p = pool.allocate();
    assert_true("first free", pool.deallocate(p) == MemoryPool::DeallocateResult::Success);
    assert_true("double free",
                pool.deallocate(p) == MemoryPool::DeallocateResult::DoubleFree);
}

void test_alignment() {
    constexpr std::size_t align = 64;
    MemoryPool pool(10, 100, align, false);
    for (int i = 0; i < 50; ++i) {
        void* p = pool.allocate();
        assert_true("align", (reinterpret_cast<std::uintptr_t>(p) % align) == 0);
        pool.deallocate(p);
    }
}

template <typename Clock = std::chrono::steady_clock>
double seconds_since(typename Clock::time_point start) {
    using namespace std::chrono;
    return duration<double>(Clock::now() - start).count();
}

void benchmark(std::size_t iterations, std::size_t block_size, std::size_t pool_blocks) {
    constexpr std::size_t kAlign = alignof(std::max_align_t);
    MemoryPool pool(block_size, pool_blocks, kAlign, false);

    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        void* p = pool.allocate();
        (void)std::memset(p, 0, block_size);
        pool.deallocate(p);
    }
    const double pool_sec = seconds_since(t0);

    t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        void* p = std::malloc(block_size);
        (void)std::memset(p, 0, block_size);
        std::free(p);
    }
    const double malloc_sec = seconds_since(t0);

    t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto* p = new std::byte[block_size];
        (void)std::memset(p, 0, block_size);
        delete[] p;
    }
    const double new_sec = seconds_since(t0);

    std::cout << "\n--- benchmark (iterations=" << iterations << ", block_size=" << block_size
              << ") ---\n";
    std::cout << "MemoryPool:  " << pool_sec << " s\n";
    std::cout << "malloc/free: " << malloc_sec << " s\n";
    std::cout << "new/delete:  " << new_sec << " s\n";
    if (pool_sec > 0) {
        std::cout << "speedup vs malloc: " << (malloc_sec / pool_sec) << "x\n";
    }
}

} // namespace

int main() {
    std::cout << "MemoryPool smoke tests\n";
    test_basic_round_trip();
    test_exhaustion_returns_null();
    test_invalid_pointer();
    test_double_free();
    test_alignment();

    benchmark(2'000'000, 32, 1);
    benchmark(2'000'000, 64, 1);

    std::cout << "\nAll tests passed.\n";
    return 0;
}
