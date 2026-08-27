#include "custom_memory/MemoryPool.hpp"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <new>
#include <thread>

namespace {

custom_memory::MemoryError reported_error =
    custom_memory::MemoryError::invalid_pointer;
bool error_reported = false;

void recordError(custom_memory::MemoryError error, const void*) noexcept {
    reported_error = error;
    error_reported = true;
}

}

int main() {
    auto& pool = custom_memory::MemoryPool::instance();
    assert(pool.initialize(1024 * 1024));
    pool.setErrorHandler(recordError);

    void* first = pool.allocate(128);
    void* first_guard = pool.allocate(32);
    void* second = pool.allocate(256);
    void* second_guard = pool.allocate(32);
    assert(pool.owns(first));
    assert(pool.owns(second));

    pool.deallocate(first);
    pool.deallocate(second);
    void* best_fit = pool.allocate(96);
    assert(best_fit == first);
    pool.deallocate(best_fit);
    pool.deallocate(first_guard);
    pool.deallocate(second_guard);
    assert(pool.statistics().live_allocations == 0);

    void* combined = pool.allocate(512);
    assert(pool.owns(combined));
    pool.deallocate(combined);

    void* adjacent_first = pool.allocate(1024);
    void* adjacent_second = pool.allocate(1024);
    void* adjacent_guard = pool.allocate(64);
    void* fillers[1024]{};
    std::size_t filler_count = 0;
    try {
        while (filler_count < 1024) {
            fillers[filler_count++] = pool.allocate(64);
        }
    } catch (const std::bad_alloc&) {
    }

    pool.deallocate(adjacent_first);
    pool.deallocate(adjacent_second);
    void* coalesced = pool.allocate(1800);
    assert(pool.owns(coalesced));
    pool.deallocate(coalesced);
    pool.deallocate(adjacent_guard);
    while (filler_count > 0) {
        pool.deallocate(fillers[--filler_count]);
    }

    constexpr std::size_t varied_count = 256;
    constexpr std::size_t varied_sizes[] = {
        24, 48, 96, 160, 320, 640, 1280
    };
    void* varied[varied_count]{};
    for (std::size_t round = 0; round < 64; ++round) {
        for (std::size_t index = 0; index < varied_count; ++index) {
            varied[index] = pool.allocate(
                varied_sizes[(index + round) % std::size(varied_sizes)]
            );
        }
        for (std::size_t index = 0; index < varied_count; index += 2) {
            pool.deallocate(varied[index]);
        }
        for (std::size_t index = 1; index < varied_count; index += 2) {
            pool.deallocate(varied[index]);
        }
    }
    assert(pool.statistics().live_allocations == 0);

    std::thread workers[4];
    for (std::size_t worker = 0; worker < std::size(workers); ++worker) {
        workers[worker] = std::thread([&pool, worker] {
            for (std::size_t iteration = 0; iteration < 4096; ++iteration) {
                const std::size_t bytes =
                    16 + ((iteration + worker) % 32) * 24;
                void* pointer = pool.allocate(bytes);
                pool.deallocate(pointer);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    assert(pool.statistics().live_allocations == 0);

    auto* bytes = static_cast<std::byte*>(pool.allocate(8));
    bytes[8] = std::byte{0};
    pool.deallocate(bytes);
    assert(error_reported);
    assert(reported_error == custom_memory::MemoryError::rear_canary_corrupted);

    pool.setErrorHandler(nullptr);
    assert(pool.shutdown());
}
