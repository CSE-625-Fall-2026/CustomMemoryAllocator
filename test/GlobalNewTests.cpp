#include "custom_memory/GlobalNew.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

int main() {
    int* before_initialization = new int(11);
    assert(!custom_memory::owns(before_initialization));

    constexpr std::size_t pool_size = 1024 * 1024;
    assert(custom_memory::initialize(pool_size));
    delete before_initialization;

    int* value = new int(42);
    assert(custom_memory::owns(value));
    assert(*value == 42);
    delete value;

    struct alignas(64) AlignedValue {
        int value;
    };

    AlignedValue* aligned = new AlignedValue{17};
    assert(custom_memory::owns(aligned));
    assert(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
    delete aligned;

    int* values = new int[128]{};
    assert(custom_memory::owns(values));
    values[127] = 29;
    assert(values[127] == 29);
    delete[] values;

    AlignedValue* aligned_values = new AlignedValue[8]{};
    assert(custom_memory::owns(aligned_values));
    assert(reinterpret_cast<std::uintptr_t>(aligned_values) % 64 == 0);
    delete[] aligned_values;

    int* nothrow_value = new (std::nothrow) int(31);
    assert(nothrow_value != nullptr);
    assert(custom_memory::owns(nothrow_value));
    delete nothrow_value;

    void* sized = ::operator new(48);
    assert(custom_memory::owns(sized));
    ::operator delete(sized, std::size_t{48});

    void* aligned_sized = ::operator new(96, std::align_val_t{64});
    assert(custom_memory::owns(aligned_sized));
    ::operator delete(aligned_sized, std::size_t{96}, std::align_val_t{64});

    assert(custom_memory::statistics().live_allocations == 0);
    assert(custom_memory::shutdown());

    int* after_shutdown = new int(37);
    assert(!custom_memory::owns(after_shutdown));
    delete after_shutdown;
}
