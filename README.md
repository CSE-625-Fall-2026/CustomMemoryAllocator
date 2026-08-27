# CustomMemoryAllocator

## Build and test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Changelog

- v1.0.0 uses one free list with first fit allocation
- v2.0.0 adds best fit allocation
- v3.0.0 adds size segregated free lists
- v4.0.0 adds fixed thread local caches
- v5.0.0 adds coalescing only when needed
- v6.0.0 adds batch refills and small block slabs
- v7.0.0 adds adaptive cache limits
