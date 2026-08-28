#include "custom_memory/MemoryPool.hpp"

#include <cstring>
#include <limits>
#include <new>

#include <sys/mman.h>
#include <unistd.h>

namespace custom_memory {
namespace {

constexpr std::uint64_t live_block_magic = 0x434D414C4C4F4341ULL;
constexpr std::uint64_t free_block_magic = 0x46524545424C4F43ULL;
constexpr std::uint64_t retired_block_magic = 0x5245544952454442ULL;

bool isPowerOfTwo(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool addWouldOverflow(std::uintptr_t value, std::size_t increment) noexcept {
    return increment > std::numeric_limits<std::uintptr_t>::max() - value;
}

std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t alignDown(std::size_t value, std::size_t alignment) noexcept {
    return value & ~(alignment - 1);
}

void storeCanary(void* address, std::uint32_t value) noexcept {
    std::memcpy(address, &value, sizeof(value));
}

std::uint32_t loadCanary(const void* address) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

}

namespace detail {

struct alignas(std::max_align_t) BlockHeader {
    std::size_t total_size{0};
    std::size_t previous_size{0};
    std::size_t requested_size{0};
    void* user_pointer{nullptr};
    BlockHeader* previous_free{nullptr};
    BlockHeader* next_free{nullptr};
    std::uint64_t magic{free_block_magic};
};

}

namespace {

constexpr std::size_t minimum_remainder =
    sizeof(detail::BlockHeader) + sizeof(void*) +
    2 * sizeof(std::uint32_t) + alignof(std::max_align_t);

struct Layout {
    std::byte* user{nullptr};
    std::size_t used_size{0};
};

Layout calculateLayout(
    detail::BlockHeader* block,
    std::size_t bytes,
    std::size_t alignment
) noexcept {
    const auto block_address = reinterpret_cast<std::uintptr_t>(block);
    auto cursor = block_address + sizeof(detail::BlockHeader);

    if (addWouldOverflow(
            cursor,
            sizeof(detail::BlockHeader*) + sizeof(std::uint32_t)
        )) {
        return {};
    }
    cursor += sizeof(detail::BlockHeader*) + sizeof(std::uint32_t);

    if (addWouldOverflow(cursor, alignment - 1)) {
        return {};
    }
    const auto user_address = alignUp(cursor, alignment);

    if (addWouldOverflow(user_address, bytes) ||
        addWouldOverflow(user_address + bytes, sizeof(std::uint32_t))) {
        return {};
    }
    const auto end_address = user_address + bytes + sizeof(std::uint32_t);

    if (addWouldOverflow(end_address, alignof(detail::BlockHeader) - 1)) {
        return {};
    }
    const auto aligned_end = alignUp(
        end_address,
        alignof(detail::BlockHeader)
    );
    const auto used_size = aligned_end - block_address;

    if (used_size > block->total_size) {
        return {};
    }
    return {
        reinterpret_cast<std::byte*>(user_address),
        static_cast<std::size_t>(used_size)
    };
}

}

MemoryPool& MemoryPool::instance() noexcept {
    alignas(MemoryPool) static std::byte storage[sizeof(MemoryPool)];
    static MemoryPool* pool =
        ::new (static_cast<void*>(storage)) MemoryPool{};
    return *pool;
}

bool MemoryPool::initialize(std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (region_ != nullptr) {
        return false;
    }

    const long page_size_result = ::sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
        return false;
    }
    const auto page_size = static_cast<std::size_t>(page_size_result);
    if (bytes > std::numeric_limits<std::size_t>::max() - page_size + 1) {
        return false;
    }
    const auto mapped_size = alignDown(bytes + page_size - 1, page_size);
    if (mapped_size < minimum_remainder) {
        return false;
    }

    void* region = ::mmap(
        nullptr,
        mapped_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (region == MAP_FAILED) {
        return false;
    }

    region_ = region;
    region_size_ = mapped_size;
    free_bins_.fill(nullptr);
    occupied_bins_ = 0;
    auto* initial_block = ::new (region_) detail::BlockHeader{};
    initial_block->total_size = mapped_size;
    insertFreeBlock(initial_block);
    statistics_ = {};
    statistics_.capacity_bytes = mapped_size;
    const auto begin = reinterpret_cast<std::uintptr_t>(region_);
    region_begin_.store(begin, std::memory_order_release);
    region_end_.store(begin + mapped_size, std::memory_order_release);
    return true;
}

bool MemoryPool::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (region_ == nullptr || statistics_.live_allocations != 0) {
        return false;
    }

    void* region = region_;
    const std::size_t region_size = region_size_;
    region_begin_.store(0, std::memory_order_release);
    region_end_.store(0, std::memory_order_release);

    if (::munmap(region, region_size) != 0) {
        const auto begin = reinterpret_cast<std::uintptr_t>(region);
        region_begin_.store(begin, std::memory_order_release);
        region_end_.store(begin + region_size, std::memory_order_release);
        return false;
    }

    region_ = nullptr;
    region_size_ = 0;
    free_bins_.fill(nullptr);
    occupied_bins_ = 0;
    statistics_ = {};
    return true;
}

bool MemoryPool::isInitialized() const noexcept {
    return region_begin_.load(std::memory_order_acquire) != 0;
}

bool MemoryPool::owns(const void* pointer) const noexcept {
    if (pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = region_begin_.load(std::memory_order_acquire);
    const auto end = region_end_.load(std::memory_order_acquire);
    return begin != 0 && address >= begin && address < end;
}

bool MemoryPool::ownsUnlocked(const void* pointer) const noexcept {
    if (region_ == nullptr || pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = reinterpret_cast<std::uintptr_t>(region_);
    return address >= begin && address < begin + region_size_;
}

Statistics MemoryPool::statistics() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

std::size_t MemoryPool::binIndex(std::size_t block_size) noexcept {
    std::size_t index = 0;
    while (block_size > 1 && index + 1 < free_bin_count) {
        block_size >>= 1;
        ++index;
    }
    return index;
}

detail::BlockHeader* MemoryPool::findBestFit(
    std::size_t bytes,
    std::size_t alignment
) const noexcept {
    if (bytes > std::numeric_limits<std::size_t>::max() -
            sizeof(detail::BlockHeader) - sizeof(detail::BlockHeader*) -
            2 * sizeof(std::uint32_t)) {
        return nullptr;
    }

    const std::size_t minimum_size = sizeof(detail::BlockHeader) +
        sizeof(detail::BlockHeader*) + 2 * sizeof(std::uint32_t) + bytes;

    for (std::size_t bin = binIndex(minimum_size);
         bin < free_bin_count;
         ++bin) {
        const std::uint64_t bit = std::uint64_t{1} << bin;
        if ((occupied_bins_ & bit) == 0) {
            continue;
        }

        detail::BlockHeader* best = nullptr;
        for (detail::BlockHeader* block = free_bins_[bin]; block != nullptr;
             block = block->next_free) {
            if (calculateLayout(block, bytes, alignment).user != nullptr &&
                (best == nullptr || block->total_size < best->total_size)) {
                best = block;
            }
        }
        if (best != nullptr) {
            return best;
        }
    }
    return nullptr;
}

void* MemoryPool::allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        bytes = 1;
    }
    if (!isPowerOfTwo(alignment)) {
        throw std::bad_alloc{};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (region_ == nullptr) {
        throw std::bad_alloc{};
    }

    detail::BlockHeader* best = findBestFit(bytes, alignment);
    if (best == nullptr) {
        throw std::bad_alloc{};
    }
    const Layout best_layout = calculateLayout(best, bytes, alignment);

    removeFreeBlock(best);
    const std::size_t remainder_size = best->total_size - best_layout.used_size;
    if (remainder_size >= minimum_remainder) {
        auto* remainder_address =
            reinterpret_cast<std::byte*>(best) + best_layout.used_size;
        auto* remainder = ::new (remainder_address) detail::BlockHeader{};
        remainder->total_size = remainder_size;
        remainder->previous_size = best_layout.used_size;
        best->total_size = best_layout.used_size;
        updateFollowingBlock(remainder);
        insertFreeBlock(remainder);
    }

    best->requested_size = bytes;
    best->user_pointer = best_layout.user;
    best->previous_free = nullptr;
    best->next_free = nullptr;
    best->magic = live_block_magic;

    std::byte* front_address = best_layout.user - sizeof(std::uint32_t);
    std::byte* owner_address = front_address - sizeof(detail::BlockHeader*);
    std::memcpy(owner_address, &best, sizeof(best));
    storeCanary(front_address, front_canary);
    storeCanary(best_layout.user + bytes, rear_canary);

    statistics_.allocated_bytes += bytes;
    ++statistics_.live_allocations;
    ++statistics_.total_allocations;
    return best_layout.user;
}

void MemoryPool::deallocate(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ownsUnlocked(pointer)) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }

    auto* user = static_cast<std::byte*>(pointer);
    const auto* region_begin = static_cast<std::byte*>(region_);
    if (user < region_begin + sizeof(detail::BlockHeader) +
                   sizeof(detail::BlockHeader*) + sizeof(std::uint32_t)) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }

    std::byte* front_address = user - sizeof(std::uint32_t);
    std::byte* owner_address =
        front_address - sizeof(detail::BlockHeader*);
    detail::BlockHeader* block = nullptr;
    std::memcpy(&block, owner_address, sizeof(block));

    if (!ownsUnlocked(block)) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }
    if (block->magic == free_block_magic ||
        block->magic == retired_block_magic) {
        report(MemoryError::double_free, pointer);
        return;
    }
    if (block->magic != live_block_magic || block->user_pointer != pointer) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }

    const bool front_valid = loadCanary(front_address) == front_canary;
    const bool rear_valid =
        loadCanary(user + block->requested_size) == rear_canary;
    if (!front_valid && !rear_valid) {
        report(MemoryError::both_canaries_corrupted, pointer);
    } else if (!front_valid) {
        report(MemoryError::front_canary_corrupted, pointer);
    } else if (!rear_valid) {
        report(MemoryError::rear_canary_corrupted, pointer);
    }

    statistics_.allocated_bytes -= block->requested_size;
    --statistics_.live_allocations;
    ++statistics_.total_deallocations;

    block->requested_size = 0;
    block->user_pointer = nullptr;
    block->magic = free_block_magic;
    coalesce(block);
}

void MemoryPool::setErrorHandler(ErrorHandler handler) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    error_handler_ = handler == nullptr ? defaultErrorHandler : handler;
}

detail::BlockHeader* MemoryPool::previousPhysicalBlock(
    detail::BlockHeader* block
) const noexcept {
    if (block->previous_size == 0) {
        return nullptr;
    }
    return reinterpret_cast<detail::BlockHeader*>(
        reinterpret_cast<std::byte*>(block) - block->previous_size
    );
}

detail::BlockHeader* MemoryPool::nextPhysicalBlock(
    detail::BlockHeader* block
) const noexcept {
    auto* next_address =
        reinterpret_cast<std::byte*>(block) + block->total_size;
    auto* region_end = static_cast<std::byte*>(region_) + region_size_;
    if (next_address >= region_end) {
        return nullptr;
    }
    return reinterpret_cast<detail::BlockHeader*>(next_address);
}

void MemoryPool::updateFollowingBlock(detail::BlockHeader* block) noexcept {
    if (detail::BlockHeader* next = nextPhysicalBlock(block)) {
        next->previous_size = block->total_size;
    }
}

void MemoryPool::insertFreeBlock(detail::BlockHeader* block) noexcept {
    const std::size_t bin = binIndex(block->total_size);
    block->magic = free_block_magic;
    block->previous_free = nullptr;
    block->next_free = free_bins_[bin];
    if (block->next_free != nullptr) {
        block->next_free->previous_free = block;
    }
    free_bins_[bin] = block;
    occupied_bins_ |= std::uint64_t{1} << bin;
}

void MemoryPool::removeFreeBlock(detail::BlockHeader* block) noexcept {
    const std::size_t bin = binIndex(block->total_size);
    if (block->previous_free != nullptr) {
        block->previous_free->next_free = block->next_free;
    } else {
        free_bins_[bin] = block->next_free;
    }
    if (block->next_free != nullptr) {
        block->next_free->previous_free = block->previous_free;
    }
    block->previous_free = nullptr;
    block->next_free = nullptr;
    if (free_bins_[bin] == nullptr) {
        occupied_bins_ &= ~(std::uint64_t{1} << bin);
    }
}

detail::BlockHeader* MemoryPool::coalesce(
    detail::BlockHeader* block
) noexcept {
    detail::BlockHeader* previous = previousPhysicalBlock(block);
    if (previous != nullptr && previous->magic == free_block_magic) {
        removeFreeBlock(previous);
        previous->total_size += block->total_size;
        block->magic = retired_block_magic;
        block = previous;
    }

    detail::BlockHeader* next = nextPhysicalBlock(block);
    if (next != nullptr && next->magic == free_block_magic) {
        removeFreeBlock(next);
        block->total_size += next->total_size;
        next->magic = retired_block_magic;
    }
    updateFollowingBlock(block);
    insertFreeBlock(block);
    return block;
}

void MemoryPool::report(MemoryError error, const void* pointer) const noexcept {
    error_handler_(error, pointer);
}

void MemoryPool::defaultErrorHandler(
    MemoryError error,
    const void*
) noexcept {
    const char* message =
        "CustomMemoryAllocator: invalid memory operation\n";
    switch (error) {
        case MemoryError::front_canary_corrupted:
            message = "CustomMemoryAllocator: front canary corrupted\n";
            break;
        case MemoryError::rear_canary_corrupted:
            message = "CustomMemoryAllocator: rear canary corrupted\n";
            break;
        case MemoryError::both_canaries_corrupted:
            message = "CustomMemoryAllocator: both canaries corrupted\n";
            break;
        case MemoryError::double_free:
            message = "CustomMemoryAllocator: double free\n";
            break;
        case MemoryError::invalid_pointer:
            break;
    }
    static_cast<void>(::write(STDERR_FILENO, message, std::strlen(message)));
}

}
