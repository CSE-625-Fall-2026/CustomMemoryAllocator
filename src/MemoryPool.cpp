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
    free_head_ = ::new (region_) detail::BlockHeader{};
    free_head_->total_size = mapped_size;
    statistics_ = {};
    statistics_.capacity_bytes = mapped_size;
    return true;
}

bool MemoryPool::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (region_ == nullptr || statistics_.live_allocations != 0) {
        return false;
    }

    if (::munmap(region_, region_size_) != 0) {
        return false;
    }

    region_ = nullptr;
    region_size_ = 0;
    free_head_ = nullptr;
    statistics_ = {};
    return true;
}

bool MemoryPool::isInitialized() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return region_ != nullptr;
}

bool MemoryPool::owns(const void* pointer) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ownsUnlocked(pointer);
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

    detail::BlockHeader* best = nullptr;
    Layout best_layout{};
    for (detail::BlockHeader* block = free_head_; block != nullptr;
         block = block->next_free) {
        const Layout layout = calculateLayout(block, bytes, alignment);
        if (layout.user != nullptr) {
            best = block;
            best_layout = layout;
            break;
        }
    }
    if (best == nullptr) {
        throw std::bad_alloc{};
    }

    removeFreeBlock(best);
    const std::size_t remainder_size = best->total_size - best_layout.used_size;
    if (remainder_size >= minimum_remainder) {
        auto* remainder_address =
            reinterpret_cast<std::byte*>(best) + best_layout.used_size;
        auto* remainder = ::new (remainder_address) detail::BlockHeader{};
        remainder->total_size = remainder_size;
        best->total_size = best_layout.used_size;
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
    insertFreeBlock(block);
    coalesce(block);
}

void MemoryPool::setErrorHandler(ErrorHandler handler) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    error_handler_ = handler == nullptr ? defaultErrorHandler : handler;
}

void MemoryPool::insertFreeBlock(detail::BlockHeader* block) noexcept {
    block->magic = free_block_magic;
    block->previous_free = nullptr;
    block->next_free = nullptr;

    if (free_head_ == nullptr) {
        free_head_ = block;
        return;
    }

    detail::BlockHeader* current = free_head_;
    detail::BlockHeader* previous = nullptr;
    const auto block_address = reinterpret_cast<std::uintptr_t>(block);
    while (current != nullptr &&
           reinterpret_cast<std::uintptr_t>(current) < block_address) {
        previous = current;
        current = current->next_free;
    }

    block->previous_free = previous;
    block->next_free = current;
    if (previous != nullptr) {
        previous->next_free = block;
    } else {
        free_head_ = block;
    }
    if (current != nullptr) {
        current->previous_free = block;
    }
}

void MemoryPool::removeFreeBlock(detail::BlockHeader* block) noexcept {
    if (block->previous_free != nullptr) {
        block->previous_free->next_free = block->next_free;
    } else {
        free_head_ = block->next_free;
    }
    if (block->next_free != nullptr) {
        block->next_free->previous_free = block->previous_free;
    }
    block->previous_free = nullptr;
    block->next_free = nullptr;
}

detail::BlockHeader* MemoryPool::coalesce(
    detail::BlockHeader* block
) noexcept {
    detail::BlockHeader* previous = block->previous_free;
    if (previous != nullptr &&
        reinterpret_cast<std::byte*>(previous) + previous->total_size ==
            reinterpret_cast<std::byte*>(block)) {
        previous->total_size += block->total_size;
        previous->next_free = block->next_free;
        if (block->next_free != nullptr) {
            block->next_free->previous_free = previous;
        }
        block->magic = retired_block_magic;
        block = previous;
    }

    detail::BlockHeader* next = block->next_free;
    if (next != nullptr &&
        reinterpret_cast<std::byte*>(block) + block->total_size ==
            reinterpret_cast<std::byte*>(next)) {
        block->total_size += next->total_size;
        block->next_free = next->next_free;
        if (next->next_free != nullptr) {
            next->next_free->previous_free = block;
        }
        next->magic = retired_block_magic;
    }
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
