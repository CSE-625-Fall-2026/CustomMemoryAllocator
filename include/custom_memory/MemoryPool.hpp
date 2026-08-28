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
    static constexpr std::size_t free_bin_count =
        std::numeric_limits<std::size_t>::digits;

    MemoryPool() noexcept = default;
    ~MemoryPool() = default;

    static void defaultErrorHandler(
        MemoryError error,
        const void* pointer
    ) noexcept;

    [[nodiscard]] bool ownsUnlocked(const void* pointer) const noexcept;
    [[nodiscard]] detail::BlockHeader* findBestFit(
        std::size_t bytes,
        std::size_t alignment
    ) const noexcept;
    [[nodiscard]] detail::BlockHeader* previousPhysicalBlock(
        detail::BlockHeader* block
    ) const noexcept;
    [[nodiscard]] detail::BlockHeader* nextPhysicalBlock(
        detail::BlockHeader* block
    ) const noexcept;
    static std::size_t binIndex(std::size_t block_size) noexcept;
    void insertFreeBlock(detail::BlockHeader* block) noexcept;
    void removeFreeBlock(detail::BlockHeader* block) noexcept;
    detail::BlockHeader* coalesce(detail::BlockHeader* block) noexcept;
    void updateFollowingBlock(detail::BlockHeader* block) noexcept;
    void report(MemoryError error, const void* pointer) const noexcept;

    mutable std::mutex mutex_;
    void* region_{nullptr};
    std::size_t region_size_{0};
    std::atomic<std::uintptr_t> region_begin_{0};
    std::atomic<std::uintptr_t> region_end_{0};
    std::array<detail::BlockHeader*, free_bin_count> free_bins_{};
    std::uint64_t occupied_bins_{0};
    Statistics statistics_{};
    ErrorHandler error_handler_{defaultErrorHandler};
};

}

#endif
