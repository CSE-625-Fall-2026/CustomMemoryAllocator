# CustomMemoryAllocator

## Build and test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Global allocation

The global `new` target replaces ordinary `new`, `new[]`, `delete`, and
`delete[]` after the caller initializes the allocator. This includes ordinary
allocations made inside dependencies used by that caller.

## Changelog

- v1.0.0 uses one ordered free list, first fit allocation, and immediate merging
- v2.0.0 searches for the smallest usable block to reduce wasted space
- v3.0.0 groups free blocks by size to reduce search work
- v4.0.0 caches small blocks per thread to avoid repeated global locking
- v5.0.0 merges adjacent blocks only when an allocation needs more space
- v6.0.0 refills caches in batches and creates reusable small block slabs
- v7.0.0 grows busy cache classes while limiting retained memory
