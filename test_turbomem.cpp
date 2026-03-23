#include "turbomem.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

struct alignas(64) PacketBuffer {
  std::uint64_t seq = 0;
  std::array<std::byte, 128> payload{};
};

int main() {
  using turbomem::PoolOptions;
  using turbomem::TurboMemPool;

  {
    TurboMemPool<PacketBuffer> pool(PoolOptions{.capacity = 256, .local_cache_capacity = 16, .bulk_size = 8, .numa_node = std::nullopt, .cpu_affinity = std::nullopt, .request_thp = true, .zero_memory = false});
    PacketBuffer* packet = pool.create();
    assert(packet != nullptr);
    packet->seq = 42;
    assert(packet->seq == 42);
    pool.destroy(packet);
    pool.flush_local_cache_for_current_thread();

    auto stats = pool.stats();
    assert(stats.capacity == 256);
    assert(stats.object_size == sizeof(PacketBuffer));
    assert(stats.bulk_size == 8);
    assert(stats.local_cache_capacity == 16);
    assert(stats.thp_requested);
  }

  {
    constexpr std::size_t kCapacity = 4096;
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kIterations = 20000;
    TurboMemPool<PacketBuffer> pool(PoolOptions{.capacity = kCapacity, .local_cache_capacity = 64, .bulk_size = 32, .numa_node = std::nullopt, .cpu_affinity = std::nullopt, .request_thp = true, .zero_memory = false});
    std::atomic<std::size_t> allocations{0};

    auto worker = [&]() {
      for (std::size_t i = 0; i < kIterations; ++i) {
        PacketBuffer* packet = nullptr;
        while ((packet = pool.allocate()) == nullptr) {
          std::this_thread::yield();
        }
        packet->seq = i;
        ++allocations;
        pool.deallocate(packet);
      }
      pool.flush_local_cache_for_current_thread();
    };

    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
      threads.emplace_back(worker);
    }
    for (auto& thread : threads) {
      thread.join();
    }

    assert(allocations == kThreads * kIterations);
  }

  std::cout << "TurboMem tests passed\n";
  return 0;
}
