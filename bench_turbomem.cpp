#include "turbomem.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;

struct BenchOptions {
  std::string allocator = "turbomem";
  std::string scenario = "packet-burst";
  std::size_t threads = 4;
  std::size_t operations_per_thread = 200000;
  std::size_t batch_size = 32;
  std::size_t pool_capacity = 1 << 15;
  std::size_t local_cache_capacity = 64;
  std::size_t bulk_size = 32;
  std::size_t object_size = 256;
  std::size_t sample_stride = 128;
  std::size_t hold_ms = 0;
  bool request_thp = true;
  bool zero_memory = false;
  bool json = false;
};

struct BenchResult {
  std::string allocator;
  std::string scenario;
  std::size_t threads = 0;
  std::size_t object_size = 0;
  std::size_t total_ops = 0;
  double seconds = 0.0;
  double ops_per_sec = 0.0;
  double alloc_p50_ns = 0.0;
  double alloc_p95_ns = 0.0;
  double alloc_p99_ns = 0.0;
  double cycle_p50_ns = 0.0;
  double cycle_p95_ns = 0.0;
  double cycle_p99_ns = 0.0;
  turbomem::PoolStats stats{};
};

double percentile(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double pos = (values.size() - 1) * p;
  const auto idx = static_cast<std::size_t>(pos);
  const double frac = pos - idx;
  if (idx + 1 >= values.size()) {
    return values[idx];
  }
  return values[idx] * (1.0 - frac) + values[idx + 1] * frac;
}

struct MallocAllocator {
  explicit MallocAllocator(std::size_t object_size) : object_size_(object_size) {}

  void* allocate() {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 64, object_size_) != 0) {
      return nullptr;
    }
    return ptr;
  }

  void deallocate(void* ptr) { std::free(ptr); }

  turbomem::PoolStats stats() const noexcept {
    turbomem::PoolStats stats;
    stats.object_size = object_size_;
    stats.object_alignment = 64;
    return stats;
  }

 private:
  std::size_t object_size_;
};

template <std::size_t Size>
struct Packet {
  alignas(64) std::array<std::byte, Size> payload{};
};

template <typename PoolT, typename ValueT>
BenchResult run_packet_burst(PoolT& pool, const BenchOptions& options, const std::string& allocator_name) {
  std::vector<std::vector<double>> alloc_samples_by_thread(options.threads);
  std::vector<std::vector<double>> cycle_samples_by_thread(options.threads);
  const auto start = Clock::now();

  auto worker = [&](std::size_t worker_index) {
    auto& alloc_samples = alloc_samples_by_thread[worker_index];
    auto& cycle_samples = cycle_samples_by_thread[worker_index];
    std::vector<ValueT*> batch;
    batch.reserve(options.batch_size);
    std::minstd_rand rng(static_cast<unsigned int>(worker_index + 1));

    for (std::size_t op = 0; op < options.operations_per_thread; op += options.batch_size) {
      batch.clear();
      const auto cycle_begin = Clock::now();
      for (std::size_t i = 0; i < options.batch_size && op + i < options.operations_per_thread; ++i) {
        const auto alloc_begin = Clock::now();
        ValueT* obj = nullptr;
        while ((obj = static_cast<ValueT*>(pool.allocate())) == nullptr) {
          std::this_thread::yield();
        }
        const auto alloc_end = Clock::now();
        if (((op + i) % options.sample_stride) == 0) {
          alloc_samples.push_back(std::chrono::duration<double, std::nano>(alloc_end - alloc_begin).count());
        }
        const auto index = static_cast<std::size_t>(rng() % sizeof(obj->payload));
        obj->payload[index] = std::byte{static_cast<unsigned char>((worker_index + i) & 0xff)};
        batch.push_back(obj);
      }
      for (auto* obj : batch) {
        pool.deallocate(obj);
      }
      if constexpr (requires { pool.flush_local_cache_for_current_thread(); }) {
        pool.flush_local_cache_for_current_thread();
      }
      const auto cycle_end = Clock::now();
      if ((op % options.sample_stride) == 0) {
        cycle_samples.push_back(std::chrono::duration<double, std::nano>(cycle_end - cycle_begin).count());
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(options.threads);
  for (std::size_t i = 0; i < options.threads; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto& thread : threads) {
    thread.join();
  }

  const auto end = Clock::now();
  if (options.hold_ms != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.hold_ms));
  }

  std::vector<double> alloc_samples;
  std::vector<double> cycle_samples;
  for (auto& values : alloc_samples_by_thread) {
    alloc_samples.insert(alloc_samples.end(), values.begin(), values.end());
  }
  for (auto& values : cycle_samples_by_thread) {
    cycle_samples.insert(cycle_samples.end(), values.begin(), values.end());
  }

  BenchResult result;
  result.allocator = allocator_name;
  result.scenario = options.scenario;
  result.threads = options.threads;
  result.object_size = sizeof(ValueT);
  result.total_ops = options.threads * options.operations_per_thread;
  result.seconds = std::chrono::duration<double>(end - start).count();
  result.ops_per_sec = result.seconds == 0.0 ? 0.0 : result.total_ops / result.seconds;
  result.alloc_p50_ns = percentile(alloc_samples, 0.50);
  result.alloc_p95_ns = percentile(alloc_samples, 0.95);
  result.alloc_p99_ns = percentile(alloc_samples, 0.99);
  result.cycle_p50_ns = percentile(cycle_samples, 0.50);
  result.cycle_p95_ns = percentile(cycle_samples, 0.95);
  result.cycle_p99_ns = percentile(cycle_samples, 0.99);
  result.stats = pool.stats();
  return result;
}

template <std::size_t Size>
BenchResult dispatch_size(const BenchOptions& options) {
  using ValueT = Packet<Size>;
  if (options.allocator == "turbomem") {
    turbomem::PoolOptions pool_options;
    pool_options.capacity = options.pool_capacity;
    pool_options.local_cache_capacity = options.local_cache_capacity;
    pool_options.bulk_size = options.bulk_size;
    pool_options.request_thp = options.request_thp;
    pool_options.zero_memory = options.zero_memory;
    turbomem::TurboMemPool<ValueT> pool(pool_options);
    return run_packet_burst<turbomem::TurboMemPool<ValueT>, ValueT>(pool, options, "turbomem");
  }
  if (options.allocator == "malloc") {
    MallocAllocator allocator(sizeof(ValueT));
    return run_packet_burst<MallocAllocator, ValueT>(allocator, options, "malloc");
  }
  throw std::invalid_argument("unknown allocator: " + options.allocator);
}

BenchResult run_benchmark(const BenchOptions& options) {
  if (options.object_size <= 64) {
    return dispatch_size<64>(options);
  }
  if (options.object_size <= 256) {
    return dispatch_size<256>(options);
  }
  if (options.object_size <= 2048) {
    return dispatch_size<2048>(options);
  }
  return dispatch_size<4096>(options);
}

BenchOptions parse_args(int argc, char** argv) {
  BenchOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& name) -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument("missing value for " + name);
      }
      return argv[++i];
    };
    if (arg == "--allocator") {
      options.allocator = require_value(arg);
    } else if (arg == "--scenario") {
      options.scenario = require_value(arg);
    } else if (arg == "--threads") {
      options.threads = std::stoull(require_value(arg));
    } else if (arg == "--operations") {
      options.operations_per_thread = std::stoull(require_value(arg));
    } else if (arg == "--batch-size") {
      options.batch_size = std::stoull(require_value(arg));
    } else if (arg == "--pool-capacity") {
      options.pool_capacity = std::stoull(require_value(arg));
    } else if (arg == "--local-cache") {
      options.local_cache_capacity = std::stoull(require_value(arg));
    } else if (arg == "--bulk-size") {
      options.bulk_size = std::stoull(require_value(arg));
    } else if (arg == "--object-size") {
      options.object_size = std::stoull(require_value(arg));
    } else if (arg == "--sample-stride") {
      options.sample_stride = std::stoull(require_value(arg));
    } else if (arg == "--hold-ms") {
      options.hold_ms = std::stoull(require_value(arg));
    } else if (arg == "--json") {
      options.json = true;
    } else if (arg == "--no-thp") {
      options.request_thp = false;
    } else if (arg == "--zero-memory") {
      options.zero_memory = true;
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  return options;
}

void print_result(const BenchResult& result, bool json) {
  if (json) {
    std::cout << "{\n"
              << "  \"pid\": " << ::getpid() << ",\n"
              << "  \"allocator\": \"" << result.allocator << "\",\n"
              << "  \"scenario\": \"" << result.scenario << "\",\n"
              << "  \"threads\": " << result.threads << ",\n"
              << "  \"object_size\": " << result.object_size << ",\n"
              << "  \"total_ops\": " << result.total_ops << ",\n"
              << "  \"seconds\": " << std::fixed << std::setprecision(6) << result.seconds << ",\n"
              << "  \"ops_per_sec\": " << result.ops_per_sec << ",\n"
              << "  \"alloc_p50_ns\": " << result.alloc_p50_ns << ",\n"
              << "  \"alloc_p95_ns\": " << result.alloc_p95_ns << ",\n"
              << "  \"alloc_p99_ns\": " << result.alloc_p99_ns << ",\n"
              << "  \"cycle_p50_ns\": " << result.cycle_p50_ns << ",\n"
              << "  \"cycle_p95_ns\": " << result.cycle_p95_ns << ",\n"
              << "  \"cycle_p99_ns\": " << result.cycle_p99_ns << ",\n"
              << "  \"stats\": {\n"
              << "    \"capacity\": " << result.stats.capacity << ",\n"
              << "    \"local_cache_capacity\": " << result.stats.local_cache_capacity << ",\n"
              << "    \"bulk_size\": " << result.stats.bulk_size << ",\n"
              << "    \"thp_requested\": " << (result.stats.thp_requested ? "true" : "false") << ",\n"
              << "    \"thp_madvise_succeeded\": " << (result.stats.thp_madvise_succeeded ? "true" : "false") << ",\n"
              << "    \"numa_binding_attempted\": " << (result.stats.numa_binding_attempted ? "true" : "false") << ",\n"
              << "    \"numa_binding_succeeded\": " << (result.stats.numa_binding_succeeded ? "true" : "false") << ",\n"
              << "    \"allocate_calls\": " << result.stats.allocate_calls << ",\n"
              << "    \"deallocate_calls\": " << result.stats.deallocate_calls << ",\n"
              << "    \"local_cache_hits\": " << result.stats.local_cache_hits << ",\n"
              << "    \"global_refills\": " << result.stats.global_refills << ",\n"
              << "    \"global_drains\": " << result.stats.global_drains << ",\n"
              << "    \"global_pushes\": " << result.stats.global_pushes << ",\n"
              << "    \"global_pops\": " << result.stats.global_pops << ",\n"
              << "    \"allocation_failures\": " << result.stats.allocation_failures << "\n"
              << "  }\n"
              << "}\n";
    return;
  }

  std::cout << "pid=" << ::getpid() << '\n'
            << "allocator=" << result.allocator << '\n'
            << "scenario=" << result.scenario << '\n'
            << "threads=" << result.threads << '\n'
            << "object_size=" << result.object_size << '\n'
            << "total_ops=" << result.total_ops << '\n'
            << "seconds=" << std::fixed << std::setprecision(6) << result.seconds << '\n'
            << "ops_per_sec=" << result.ops_per_sec << '\n'
            << "alloc_p50_ns=" << result.alloc_p50_ns << '\n'
            << "alloc_p95_ns=" << result.alloc_p95_ns << '\n'
            << "alloc_p99_ns=" << result.alloc_p99_ns << '\n'
            << "cycle_p50_ns=" << result.cycle_p50_ns << '\n'
            << "cycle_p95_ns=" << result.cycle_p95_ns << '\n'
            << "cycle_p99_ns=" << result.cycle_p99_ns << '\n'
            << "pool_allocate_calls=" << result.stats.allocate_calls << '\n'
            << "pool_deallocate_calls=" << result.stats.deallocate_calls << '\n'
            << "pool_local_cache_hits=" << result.stats.local_cache_hits << '\n'
            << "pool_global_refills=" << result.stats.global_refills << '\n'
            << "pool_global_drains=" << result.stats.global_drains << '\n'
            << "pool_global_pushes=" << result.stats.global_pushes << '\n'
            << "pool_global_pops=" << result.stats.global_pops << '\n'
            << "pool_allocation_failures=" << result.stats.allocation_failures << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const BenchOptions options = parse_args(argc, argv);
    const BenchResult result = run_benchmark(options);
    print_result(result, options.json);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "bench_turbomem error: " << ex.what() << '\n';
    return 1;
  }
}
