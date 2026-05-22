/*
 * @file memory_pool.cpp
 * @brief Implementation of MemoryPool — fixed-size block allocator with freelist and validation.
 */

#include "memory_pool.hpp"

#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>

bool MemoryPool::is_power_of_two(std::size_t x) noexcept {
    return x != 0 && (x & (x - 1)) == 0;
}

std::size_t MemoryPool::round_up(std::size_t value, std::size_t align) {
    if (!is_power_of_two(align)) {
        throw std::invalid_argument("MemoryPool: alignment must be a power of two");
    }
    return (value + align - 1) & ~(align - 1);
}

std::size_t MemoryPool::next_power_of_two(std::size_t x) {
    if (x == 0 || x > std::numeric_limits<std::size_t>::max() / 2 + 1) {
        throw std::overflow_error("MemoryPool: stride normalization overflow");
    }
    std::size_t p = 1;
    while (p < x) {
        p <<= 1;
    }
    return p;
}

MemoryPool::MemoryPool(std::size_t block_size, std::size_t capacity, std::size_t alignment, bool thread_safe)
    : user_block_size_(block_size), capacity_(capacity), thread_safe_(thread_safe) {
    if (block_size == 0) {
        throw std::invalid_argument("MemoryPool: block_size must be > 0");
    }
    if (capacity == 0) {
        throw std::invalid_argument("MemoryPool: capacity must be > 0");
    }
    if (alignment < alignof(void*) || !is_power_of_two(alignment)) {
        throw std::invalid_argument("MemoryPool: alignment must be >= alignof(void*) and a power of two");
    }

    const std::size_t min_stride = block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size;
    const std::size_t stride_min = round_up(min_stride, alignment);
    // Power-of-two stride gives a valid std::align_val_t for the slab and keeps all block addresses aligned.
    stride_ = next_power_of_two(stride_min);

    // Guard overflow: capacity * stride
    if (capacity > std::numeric_limits<std::size_t>::max() / stride_) {
        throw std::overflow_error("MemoryPool: total storage size overflow");
    }

    const std::size_t total_bytes = capacity * stride_;
    // Slab must be aligned to stride_ so every block address (base + i * stride_) is stride_-aligned.
    void* raw = ::operator new[](total_bytes, std::align_val_t{stride_}, std::nothrow);
    if (!raw) {
        throw std::bad_alloc();
    }
    storage_ = std::unique_ptr<std::byte, AlignedByteDeleter>(
        static_cast<std::byte*>(raw), AlignedByteDeleter{stride_});

    in_use_.reset(new (std::nothrow) std::uint8_t[capacity]);
    if (!in_use_) {
        storage_.reset();
        throw std::bad_alloc();
    }
    for (std::size_t i = 0; i < capacity; ++i) {
        in_use_[i] = 0;
    }

    // Build free list (LIFO push order does not matter for correctness).
    free_head_ = nullptr;
    std::byte* base = storage_.get();
    for (std::size_t i = 0; i < capacity; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(base + i * stride_);
        node->next = free_head_;
        free_head_ = node;
    }
    free_count_ = capacity;
}

void* MemoryPool::allocate_unlocked() {
    if (!free_head_) {
        return nullptr;
    }

    FreeNode* node = free_head_;
    free_head_ = node->next;

    const auto idx = index_from_pointer(reinterpret_cast<const std::byte*>(node));
    in_use_[idx] = 1;
    --free_count_;

    return node;
}

void* MemoryPool::allocate() {
    if (thread_safe_) {
        std::lock_guard<std::mutex> guard(mutex_);
        return allocate_unlocked();
    }
    return allocate_unlocked();
}

MemoryPool::DeallocateResult MemoryPool::deallocate_unlocked(void* ptr) noexcept {
    auto* p = reinterpret_cast<std::byte*>(ptr);

    if (!pointer_in_pool(p)) {
        return DeallocateResult::InvalidPointer;
    }
    if (reinterpret_cast<std::uintptr_t>(p) % stride_ != 0) {
        return DeallocateResult::InvalidPointer;
    }

    const std::size_t idx = index_from_pointer(p);
    if (in_use_[idx] == 0) {
        return DeallocateResult::DoubleFree;
    }

    in_use_[idx] = 0;
    auto* node = reinterpret_cast<FreeNode*>(p);
    node->next = free_head_;
    free_head_ = node;
    ++free_count_;

    return DeallocateResult::Success;
}

MemoryPool::DeallocateResult MemoryPool::deallocate(void* ptr) noexcept {
    if (!ptr) {
        return DeallocateResult::InvalidPointer;
    }

    if (thread_safe_) {
        std::lock_guard<std::mutex> guard(mutex_);
        return deallocate_unlocked(ptr);
    }
    return deallocate_unlocked(ptr);
}

std::size_t MemoryPool::index_from_pointer(const std::byte* p) const noexcept {
    const std::byte* base = storage_.get();
    return static_cast<std::size_t>((p - base) / stride_);
}

bool MemoryPool::pointer_in_pool(const std::byte* p) const noexcept {
    const std::byte* base = storage_.get();
    const std::byte* end = base + capacity_ * stride_;
    return p >= base && p < end;
}
