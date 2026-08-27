#include "custom_memory/GlobalNew.hpp"

#include <cstdlib>
#include <new>

namespace {

void* allocateWithSystem(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        bytes = 1;
    }

    if (alignment <= alignof(std::max_align_t)) {
        if (void* pointer = std::malloc(bytes)) {
            return pointer;
        }
        throw std::bad_alloc{};
    }

    void* pointer = nullptr;
    if (::posix_memalign(&pointer, alignment, bytes) != 0) {
        throw std::bad_alloc{};
    }
    return pointer;
}

void* allocate(std::size_t bytes, std::size_t alignment) {
    auto& pool = custom_memory::MemoryPool::instance();
    if (pool.isInitialized()) {
        return pool.allocate(bytes, alignment);
    }
    return allocateWithSystem(bytes, alignment);
}

void deallocate(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }

    auto& pool = custom_memory::MemoryPool::instance();
    if (pool.owns(pointer)) {
        pool.deallocate(pointer);
    } else {
        std::free(pointer);
    }
}

}

namespace custom_memory {

bool initialize(std::size_t bytes) noexcept {
    return MemoryPool::instance().initialize(bytes);
}

bool shutdown() noexcept {
    return MemoryPool::instance().shutdown();
}

bool owns(const void* pointer) noexcept {
    return MemoryPool::instance().owns(pointer);
}

Statistics statistics() noexcept {
    return MemoryPool::instance().statistics();
}

}

void* operator new(std::size_t bytes) {
    return allocate(bytes, alignof(std::max_align_t));
}

void* operator new[](std::size_t bytes) {
    return allocate(bytes, alignof(std::max_align_t));
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    std::size_t bytes,
    const std::nothrow_t&
) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(
    std::size_t bytes,
    std::align_val_t alignment,
    const std::nothrow_t&
) noexcept {
    try {
        return ::operator new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    std::size_t bytes,
    std::align_val_t alignment,
    const std::nothrow_t&
) noexcept {
    try {
        return ::operator new[](bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept {
    deallocate(pointer);
}

void operator delete[](void* pointer) noexcept {
    deallocate(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    deallocate(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    deallocate(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    deallocate(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
    deallocate(pointer);
}

void operator delete(
    void* pointer,
    std::size_t,
    std::align_val_t
) noexcept {
    deallocate(pointer);
}

void operator delete[](
    void* pointer,
    std::size_t,
    std::align_val_t
) noexcept {
    deallocate(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    deallocate(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    deallocate(pointer);
}

void operator delete(
    void* pointer,
    std::align_val_t,
    const std::nothrow_t&
) noexcept {
    deallocate(pointer);
}

void operator delete[](
    void* pointer,
    std::align_val_t,
    const std::nothrow_t&
) noexcept {
    deallocate(pointer);
}
