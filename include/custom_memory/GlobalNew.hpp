#ifndef CUSTOM_MEMORY_ALLOCATOR_GLOBAL_NEW_HPP
#define CUSTOM_MEMORY_ALLOCATOR_GLOBAL_NEW_HPP

#include "custom_memory/MemoryPool.hpp"

#include <cstddef>

namespace custom_memory {

bool initialize(std::size_t bytes) noexcept;
bool shutdown() noexcept;

[[nodiscard]] bool owns(const void* pointer) noexcept;
[[nodiscard]] Statistics statistics() noexcept;

}

#endif
