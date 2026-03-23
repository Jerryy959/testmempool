# TurboMem-inspired Memory Pool Research Harness

This repository now contains both the allocator implementation and a reproducible benchmarking/measurement harness aimed at turning the TurboMem paper into something you can actually run, compare, and profile.

## What is in the repo

- `turbomem.hpp`: the fixed-size pool itself, with a lock-free global freelist, per-thread local caches, contiguous `mmap` backing, THP hinting, optional NUMA binding, CPU affinity hooks, and runtime counters exposed via `stats()`.
- `test_turbomem.cpp`: correctness and regression tests for basic allocation, placement-new reuse, concurrency, and cache flushing behavior.
- `bench_turbomem.cpp`: a runnable benchmark binary with a packet-burst scenario, allocator comparison (`turbomem` vs `malloc`), throughput reporting, sampled latency percentiles, and exported allocator counters.
- `scripts/run_repro_bench.py`: an experiment driver that can launch the benchmark, capture `/proc/<pid>/smaps_rollup`, `/proc/<pid>/numa_maps`, `numastat`, and optionally wrap the run with `perf stat` and/or VTune CLI.

## Implemented paper-aligned ideas

- **Lock-free global pool:** the global freelist uses an ABA-safe tagged head over slot indices.
- **Per-thread local caches:** threads allocate from local caches first, touching the global stack only for refill/drain.
- **Contiguous backing memory:** the pool allocates a single anonymous `mmap` region and slices it into cache-line-aligned slots.
- **THP auto-merging request:** the pool requests transparent huge pages with `madvise(MADV_HUGEPAGE)`.
- **NUMA-awareness hooks:** optional `mbind()` support is compiled in only when headers and `libnuma` are both present.
- **Allocator observability:** runtime counters track allocate/deallocate traffic, local-cache hits, refill/drain events, global push/pop counts, and allocation failures.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run correctness tests

```bash
./build/test_turbomem
```

## Run the benchmark directly

```bash
./build/bench_turbomem --allocator turbomem --threads 4 --operations 200000 --batch-size 32 --object-size 256 --json
./build/bench_turbomem --allocator malloc --threads 4 --operations 200000 --batch-size 32 --object-size 256 --json
```

The benchmark currently models a packet-burst style workload: each thread repeatedly allocates a burst of fixed-size packet buffers, writes to them, and returns them. This makes the paper's local-cache + bulk refill/drain design concrete for networking-style workloads.

## Run a reproducible measurement collection

```bash
python3 scripts/run_repro_bench.py --allocator turbomem --threads 4 --operations 200000 --object-size 256 --tool perf
python3 scripts/run_repro_bench.py --allocator turbomem --threads 4 --operations 200000 --object-size 256 --tool vtune
```

Artifacts are written to `artifacts/latest-run/` by default and can include:

- benchmark JSON output
- `perf stat` counters such as cycles, instructions, cache misses, and dTLB misses
- `/proc/<pid>/smaps_rollup`
- `/proc/<pid>/numa_maps`
- `numastat -p <pid>` output
- VTune CLI output when `vtune` is installed

## Suggested next experiments

- Compare `turbomem` vs `malloc` across `--threads 1,2,4,8,16`.
- Compare `--object-size 64`, `256`, and `2048` to emulate packet metadata, mbuf-like objects, and larger buffers.
- Sweep `--local-cache` and `--bulk-size` to quantify the locality/throughput tradeoff.
- Run once with THP enabled and once with `--no-thp`, then compare `perf` counters and `smaps_rollup`.
- Use `taskset`, `numactl`, or VTune's memory-access analysis around `bench_turbomem` to study NUMA/TLB/cache effects.

## DPDK-oriented integration direction

This repo still is not a drop-in DPDK mempool replacement, but it now gives you a concrete harness to evaluate the paper's ideas under a packet-burst workload that is much closer to the intended networking scenario than the previous unit-style prototype.
