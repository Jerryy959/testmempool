# TurboMem-inspired Memory Pool

This repository contains a fresh C++20 implementation of a TurboMem-style fixed-size memory pool inspired by the paper [TurboMem: High-Performance Lock-Free Memory Pool with Transparent Huge Page Auto-Merging for DPDK](https://arxiv.org/pdf/2603.18690).

## Implemented design points

- **Lock-free global pool:** free slots are stored in a Treiber stack built from atomic compare-and-swap operations.
- **Per-thread local caches:** each thread uses a private local stack for the fast path and only touches the global stack during bulk refill/drain operations.
- **Contiguous backing memory:** the pool allocates a single anonymous `mmap` region and subdivides it into cache-line-aligned slots.
- **THP auto-merging request:** the backing mapping is marked with `madvise(MADV_HUGEPAGE)` to request transparent huge page promotion.
- **NUMA-awareness hooks:** optional NUMA binding metadata and Linux `mbind()` support are included when available.
- **CPU affinity support:** callers can pin the creating thread, or pin worker threads separately, to enforce the locality model described in the paper.
- **Type-safe object lifecycle:** the pool exposes `create()`/`destroy()` wrappers around placement construction and destruction.

## Build and run

```bash
cmake -S . -B build
cmake --build build
./build/test_turbomem
```

## Notes

- The referenced paper is a 7-page preprint and reports mock benchmark numbers rather than a production-grade reference implementation.
- This repository therefore implements the paper's architectural requirements faithfully, but it does **not** claim bit-for-bit equivalence with DPDK internals or the paper's unpublished benchmark harness.
