#ifndef CUSTOM_MEMORY_ALLOCATOR_MEMORY_POOL_HPP
#define CUSTOM_MEMORY_ALLOCATOR_MEMORY_POOL_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace custom_memory {

inline constexpr std::uint32_t front_canary = 0xCAFEBABEu;
inline constexpr std::uint32_t rear_canary = 0xDEADBEEFu;

enum class MemoryError {
    front_canary_corrupted,
    rear_canary_corrupted,
    both_canaries_corrupted,
    invalid_pointer,
    double_free
};

using ErrorHandler = void (*)(MemoryError, const void*) noexcept;

struct Statistics {
    std::size_t capacity_bytes{0};
    std::size_t allocated_bytes{0};
    std::size_t live_allocations{0};
    std::size_t total_allocations{0};
    std::size_t total_deallocations{0};
};

namespace detail {
struct BlockHeader;
struct ThreadCache;
}

class MemoryPool final {
public:
    static MemoryPool& instance() noexcept;

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    bool initialize(std::size_t bytes) noexcept;
    bool shutdown() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept;
    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] Statistics statistics() const noexcept;

    void* allocate(
        std::size_t bytes,
        std::size_t alignment = alignof(std::max_align_t)
    );
    void deallocate(void* pointer) noexcept;

    void setErrorHandler(ErrorHandler handler) noexcept;

private:
    static constexpr std::size_t small_bin_quantum = 64;
    static constexpr std::size_t small_bin_count = 128;
    static constexpr std::size_t small_block_limit =
        small_bin_quantum * small_bin_count;
    static constexpr std::size_t large_bin_count =
        std::numeric_limits<std::size_t>::digits;
    static constexpr std::size_t initial_cached_blocks_per_bin = 16;
    static constexpr std::size_t maximum_cached_blocks_per_bin = 256;
    static constexpr std::size_t cache_growth_interval = 4;
    static constexpr std::size_t cache_refill_batch = 32;
    static constexpr std::size_t cache_flush_batch = 32;
    static constexpr std::size_t maximum_thread_cache_bytes = 4 * 1024 * 1024;
    static constexpr std::size_t large_allocation_threshold = 64 * 1024;

    friend struct detail::ThreadCache;

    MemoryPool() noexcept = default;
    ~MemoryPool() = default;

    static void defaultErrorHandler(
        MemoryError error,
        const void* pointer
    ) noexcept;

    [[nodiscard]] bool ownsUnlocked(const void* pointer) const noexcept;
    [[nodiscard]] detail::ThreadCache* registerThreadCache() noexcept;
    [[nodiscard]] detail::BlockHeader* takeCachedBlock(
        detail::ThreadCache& cache,
        std::size_t bytes,
        std::size_t alignment
    ) noexcept;
    [[nodiscard]] detail::BlockHeader* findBestFit(
        std::size_t bytes,
        std::size_t alignment,
        std::size_t minimum_block_size = 0
    ) const noexcept;
    [[nodiscard]] detail::BlockHeader* nextPhysicalBlock(
        detail::BlockHeader* block
    ) const noexcept;
    static std::size_t smallBinIndex(std::size_t block_size) noexcept;
    static std::size_t largeBinIndex(std::size_t block_size) noexcept;
    [[nodiscard]] std::size_t effectiveAlignment(
        std::size_t bytes,
        std::size_t alignment
    ) const noexcept;
    void* activateBlock(
        detail::BlockHeader* block,
        std::size_t bytes,
        std::size_t alignment
    ) noexcept;
    void cacheBlock(
        detail::ThreadCache& cache,
        detail::BlockHeader* block
    ) noexcept;
    void pushCachedBlock(
        detail::ThreadCache& cache,
        detail::BlockHeader* block
    ) noexcept;
    [[nodiscard]] detail::BlockHeader* reserveFreeBlock(
        detail::BlockHeader* block,
        std::size_t bytes,
        std::size_t alignment,
        std::size_t preferred_size
    ) noexcept;
    [[nodiscard]] detail::BlockHeader* refillSmallCacheUnlocked(
        detail::ThreadCache& cache,
        detail::BlockHeader* first,
        std::size_t bytes,
        std::size_t alignment
    ) noexcept;
    void flushCacheBinUnlocked(
        detail::ThreadCache& cache,
        std::size_t bin,
        std::size_t count
    ) noexcept;
    void flushThreadCacheUnlocked(detail::ThreadCache& cache) noexcept;
    void releaseThreadCache(detail::ThreadCache& cache) noexcept;
    void insertFreeBlock(detail::BlockHeader* block) noexcept;
    void removeFreeBlock(detail::BlockHeader* block) noexcept;
    void coalesceFreeBlocksUnlocked() noexcept;
    void report(MemoryError error, const void* pointer) const noexcept;

    mutable std::mutex mutex_;
    void* region_{nullptr};
    std::size_t region_size_{0};
    std::size_t page_size_{0};
    std::size_t active_thread_caches_{0};
    std::atomic<std::uintptr_t> region_begin_{0};
    std::atomic<std::uintptr_t> region_end_{0};
    std::array<detail::BlockHeader*, small_bin_count> small_bins_{};
    std::array<std::uint64_t, small_bin_count / 64> occupied_small_bins_{};
    std::array<detail::BlockHeader*, large_bin_count> large_bins_{};
    std::uint64_t occupied_large_bins_{0};
    std::atomic<std::size_t> capacity_bytes_{0};
    std::atomic<std::size_t> allocated_bytes_{0};
    std::atomic<std::size_t> live_allocations_{0};
    std::atomic<std::size_t> total_allocations_{0};
    std::atomic<std::size_t> total_deallocations_{0};
    std::atomic<ErrorHandler> error_handler_{defaultErrorHandler};
};

}

#endif
