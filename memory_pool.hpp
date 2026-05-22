/**
 * @file memory_pool.hpp
 * @brief Fixed-size block memory pool for high-frequency allocate/free of same-sized objects.
 *
 * Design:
 * - One contiguous backing buffer (no per-allocation heap calls after construction).
 * - Intrusive singly linked free list: each free block stores a FreeNode* to the next free block.
 * - Stride per block is at least max(user block size, sizeof(void*)), rounded up to alignment, so
 *   the free-list pointer fits and returned pointers satisfy alignment.
 * - Optional std::mutex protects allocate/deallocate for multi-threaded use.
 *
 * Error handling:
 * - allocate() returns nullptr when the pool is exhausted (fixed capacity; no implicit growth).
 * - deallocate() returns a DeallocateResult to signal invalid pointer or double-free.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>

class MemoryPool {
public:
    enum class DeallocateResult : std::uint8_t {
        Success = 0,
        InvalidPointer,
        DoubleFree,
    };

    /**
     * @param block_size   Logical payload size per block (must be > 0).
     * @param capacity     Number of blocks in the pool.
     * @param alignment    Minimum alignment for returned pointers (power of two, >= alignof(void*)).
     * @param thread_safe  If true, allocate/deallocate use an internal mutex.
     */
    MemoryPool(std::size_t block_size, std::size_t capacity, std::size_t alignment = alignof(std::max_align_t),
               bool thread_safe = true);

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    ~MemoryPool() = default;

    /** Returns a block from the free list, or nullptr if empty. */
    void* allocate();

    /**
     * Returns a block to the pool.
     * @return Success, InvalidPointer (not from this pool or misaligned), or DoubleFree.
     */
    DeallocateResult deallocate(void* ptr) noexcept;

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t free_count() const noexcept { return free_count_; }
    std::size_t block_size() const noexcept { return user_block_size_; }
    std::size_t stride() const noexcept { return stride_; }
    bool thread_safe() const noexcept { return thread_safe_; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    static std::size_t round_up(std::size_t value, std::size_t align);
    static bool is_power_of_two(std::size_t x) noexcept;
    /** Smallest power-of-two >= x (x must be > 0). Valid alignment for ::operator new[]. */
    static std::size_t next_power_of_two(std::size_t x);

    std::size_t index_from_pointer(const std::byte* p) const noexcept;
    bool pointer_in_pool(const std::byte* p) const noexcept;

    void* allocate_unlocked();
    DeallocateResult deallocate_unlocked(void* ptr) noexcept;

    struct AlignedByteDeleter {
        std::size_t alignment = alignof(std::max_align_t);
        void operator()(std::byte* p) const noexcept {
            if (p) {
                ::operator delete[](p, std::align_val_t{alignment});
            }
        }
    };

    std::size_t user_block_size_{};
    std::size_t capacity_{};
    std::size_t stride_{};
    std::size_t free_count_{};
    bool thread_safe_{};

    std::unique_ptr<std::byte, AlignedByteDeleter> storage_;
    std::unique_ptr<std::uint8_t[]> in_use_; // 1 = handed out, 0 = on free list

    FreeNode* free_head_{nullptr};
    mutable std::mutex mutex_;
};
