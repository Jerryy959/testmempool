#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef TURBOMEM_HAS_LIBNUMA
#if defined(__linux__) && __has_include(<numa.h>) && __has_include(<numaif.h>)
#define TURBOMEM_HAS_LIBNUMA 1
#else
#define TURBOMEM_HAS_LIBNUMA 0
#endif
#endif

#if TURBOMEM_HAS_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif

namespace turbomem {

struct PoolOptions {
  std::size_t capacity = 0;
  std::size_t local_cache_capacity = 64;
  std::size_t bulk_size = 32;
  std::optional<int> numa_node;
  std::optional<int> cpu_affinity;
  bool request_thp = true;
  bool zero_memory = false;
};

struct PoolStats {
  std::size_t capacity = 0;
  std::size_t object_size = 0;
  std::size_t object_alignment = 0;
  std::size_t stride = 0;
  std::size_t local_cache_capacity = 0;
  std::size_t bulk_size = 0;
  bool thp_requested = false;
  bool thp_madvise_succeeded = false;
  bool numa_binding_attempted = false;
  bool numa_binding_succeeded = false;
};

namespace detail {

inline std::size_t round_up(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

inline void set_affinity_for_current_thread(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (rc != 0) {
    throw std::runtime_error("pthread_setaffinity_np failed");
  }
}

class Mapping {
 public:
  Mapping() = default;

  Mapping(std::size_t bytes, bool request_thp, std::optional<int> numa_node, bool zero_memory)
      : bytes_(bytes) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* ptr = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (ptr == MAP_FAILED) {
      throw std::bad_alloc();
    }
    base_ = ptr;

    if (zero_memory) {
      std::memset(base_, 0, bytes_);
    }

#if defined(__linux__)
    if (request_thp) {
      thp_requested_ = true;
      thp_madvise_succeeded_ = (madvise(base_, bytes_, MADV_HUGEPAGE) == 0);
    }
    if (numa_node.has_value()) {
      numa_binding_attempted_ = true;
#if TURBOMEM_HAS_LIBNUMA
      if (numa_available() >= 0) {
        unsigned long mask = 1UL << static_cast<unsigned long>(*numa_node);
        const long rc = mbind(base_, bytes_, MPOL_BIND, &mask, sizeof(mask) * 8, 0);
        numa_binding_succeeded_ = (rc == 0);
      }
#endif
    }
#else
    (void)request_thp;
    (void)numa_node;
#endif
  }

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  Mapping(Mapping&& other) noexcept { *this = std::move(other); }
  Mapping& operator=(Mapping&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    reset();
    base_ = std::exchange(other.base_, nullptr);
    bytes_ = std::exchange(other.bytes_, 0);
    thp_requested_ = other.thp_requested_;
    thp_madvise_succeeded_ = other.thp_madvise_succeeded_;
    numa_binding_attempted_ = other.numa_binding_attempted_;
    numa_binding_succeeded_ = other.numa_binding_succeeded_;
    return *this;
  }

  ~Mapping() { reset(); }

  void* data() const noexcept { return base_; }
  std::size_t size() const noexcept { return bytes_; }
  bool thp_requested() const noexcept { return thp_requested_; }
  bool thp_madvise_succeeded() const noexcept { return thp_madvise_succeeded_; }
  bool numa_binding_attempted() const noexcept { return numa_binding_attempted_; }
  bool numa_binding_succeeded() const noexcept { return numa_binding_succeeded_; }

 private:
  void reset() noexcept {
    if (base_ != nullptr) {
      munmap(base_, bytes_);
    }
    base_ = nullptr;
    bytes_ = 0;
  }

  void* base_ = nullptr;
  std::size_t bytes_ = 0;
  bool thp_requested_ = false;
  bool thp_madvise_succeeded_ = false;
  bool numa_binding_attempted_ = false;
  bool numa_binding_succeeded_ = false;
};

template <typename T>
struct Slot {
  std::uint32_t next_index = 0;
  alignas(T) std::byte storage[sizeof(T)];

  T* object() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
  const T* object() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }
};

template <typename T>
class TreiberStack {
 public:
  using slot_type = Slot<T>;
  static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

  TreiberStack() = default;

  TreiberStack(slot_type* base, std::size_t stride, std::size_t capacity)
      : base_(base), stride_(stride), capacity_(capacity) {
    if (capacity_ > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - 1)) {
      throw std::invalid_argument("capacity exceeds indexed Treiber stack limit");
    }
    head_.store(pack(kInvalidIndex, 0), std::memory_order_relaxed);
  }

  void push(slot_type* slot) noexcept {
    const std::uint32_t slot_index = index_of(slot);
    std::uint64_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
      const auto [head_index, head_tag] = unpack(head);
      slot->next_index = head_index;
      const std::uint64_t desired = pack(slot_index, head_tag + 1);
      if (head_.compare_exchange_weak(head, desired, std::memory_order_release,
                                      std::memory_order_relaxed)) {
        return;
      }
    }
  }

  slot_type* pop() noexcept {
    std::uint64_t head = head_.load(std::memory_order_acquire);
    for (;;) {
      const auto [head_index, head_tag] = unpack(head);
      if (head_index == kInvalidIndex) {
        return nullptr;
      }
      slot_type* slot = pointer_from_index(head_index);
      const std::uint64_t desired = pack(slot->next_index, head_tag + 1);
      if (head_.compare_exchange_weak(head, desired, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        return slot;
      }
    }
  }

  std::size_t pop_bulk(slot_type** out, std::size_t max_count) noexcept {
    std::size_t count = 0;
    while (count < max_count) {
      slot_type* slot = pop();
      if (slot == nullptr) {
        break;
      }
      out[count++] = slot;
    }
    return count;
  }

  void push_bulk(slot_type** slots, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      push(slots[i]);
    }
  }

 private:
  static std::uint64_t pack(std::uint32_t index, std::uint32_t tag) noexcept {
    return (static_cast<std::uint64_t>(tag) << 32) | index;
  }

  static std::pair<std::uint32_t, std::uint32_t> unpack(std::uint64_t value) noexcept {
    return {static_cast<std::uint32_t>(value & 0xffffffffULL),
            static_cast<std::uint32_t>(value >> 32)};
  }

  std::uint32_t index_of(const slot_type* slot) const noexcept {
    const auto base = reinterpret_cast<const std::byte*>(base_);
    const auto current = reinterpret_cast<const std::byte*>(slot);
    const auto offset = static_cast<std::size_t>(current - base);
    return static_cast<std::uint32_t>(offset / stride_);
  }

  slot_type* pointer_from_index(std::uint32_t index) const noexcept {
    return reinterpret_cast<slot_type*>(reinterpret_cast<std::byte*>(base_) +
                                        static_cast<std::size_t>(index) * stride_);
  }

  slot_type* base_ = nullptr;
  std::size_t stride_ = 0;
  std::size_t capacity_ = 0;
  std::atomic<std::uint64_t> head_{pack(kInvalidIndex, 0)};
};

template <typename T>
class LocalCacheRegistry {
 public:
  struct Cache {
    std::vector<Slot<T>*> slots;
    const void* pool = nullptr;
    bool registered = false;
  };

  using RegisterFn = void (*)(const void*, Cache*);

  static Cache& cache_for(const void* key, std::size_t reserve, RegisterFn register_fn) {
    thread_local std::unordered_map<const void*, Cache> caches;
    auto [it, inserted] = caches.try_emplace(key);
    if (inserted) {
      it->second.slots.reserve(reserve);
      it->second.pool = key;
    }
    if (!it->second.registered) {
      register_fn(key, &it->second);
      it->second.registered = true;
    }
    return it->second;
  }

  static void clear_for(const void* key) {
    thread_local std::unordered_map<const void*, Cache> caches;
    auto it = caches.find(key);
    if (it != caches.end()) {
      it->second.slots.clear();
      it->second.registered = false;
      caches.erase(it);
    }
  }
};

}  // namespace detail

template <typename T>
class TurboMemPool {
 public:
  using value_type = T;
  using slot_type = detail::Slot<T>;

  explicit TurboMemPool(PoolOptions options)
      : options_(normalize(std::move(options))),
        stride_(detail::round_up(sizeof(slot_type), std::max<std::size_t>(alignof(slot_type), 64))),
        mapping_(stride_ * options_.capacity, options_.request_thp, options_.numa_node,
                 options_.zero_memory),
        global_(reinterpret_cast<slot_type*>(mapping_.data()), stride_, options_.capacity) {
    if (options_.cpu_affinity.has_value()) {
      detail::set_affinity_for_current_thread(*options_.cpu_affinity);
    }

    auto* base = static_cast<std::byte*>(mapping_.data());
    slots_.reserve(options_.capacity);
    for (std::size_t i = 0; i < options_.capacity; ++i) {
      auto* slot = ::new (base + i * stride_) slot_type();
      slots_.push_back(slot);
      global_.push(slot);
    }
  }

  TurboMemPool(const TurboMemPool&) = delete;
  TurboMemPool& operator=(const TurboMemPool&) = delete;

  ~TurboMemPool() {
    clear_registered_caches();
    detail::LocalCacheRegistry<T>::clear_for(this);
    for (slot_type* slot : slots_) {
      if (constructed_.load(std::memory_order_relaxed)) {
        // Best effort cleanup for objects still constructed by caller contract.
      }
      slot->~slot_type();
    }
  }

  [[nodiscard]] T* allocate() noexcept {
    auto& cache = local_cache();
    if (cache.slots.empty()) {
      refill(cache);
    }
    if (cache.slots.empty()) {
      return nullptr;
    }
    slot_type* slot = cache.slots.back();
    cache.slots.pop_back();
    return slot->object();
  }

  void deallocate(T* ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }
    slot_type* slot = slot_from_object(ptr);
    auto& cache = local_cache();
    cache.slots.push_back(slot);
    if (cache.slots.size() >= options_.local_cache_capacity) {
      drain(cache);
    }
  }

  template <typename... Args>
  [[nodiscard]] T* create(Args&&... args) {
    T* ptr = allocate();
    if (ptr == nullptr) {
      return nullptr;
    }
    ::new (static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
    constructed_.store(true, std::memory_order_relaxed);
    return ptr;
  }

  void destroy(T* ptr) noexcept(std::is_nothrow_destructible_v<T>) {
    if (ptr == nullptr) {
      return;
    }
    ptr->~T();
    deallocate(ptr);
  }

  [[nodiscard]] PoolStats stats() const noexcept {
    return PoolStats{.capacity = options_.capacity,
                     .object_size = sizeof(T),
                     .object_alignment = alignof(T),
                     .stride = stride_,
                     .local_cache_capacity = options_.local_cache_capacity,
                     .bulk_size = options_.bulk_size,
                     .thp_requested = mapping_.thp_requested(),
                     .thp_madvise_succeeded = mapping_.thp_madvise_succeeded(),
                     .numa_binding_attempted = mapping_.numa_binding_attempted(),
                     .numa_binding_succeeded = mapping_.numa_binding_succeeded()};
  }

  static void pin_current_thread(int cpu) { detail::set_affinity_for_current_thread(cpu); }

  void flush_local_cache_for_current_thread() noexcept {
    auto& cache = local_cache();
    drain_all(cache);
  }

 private:
  static PoolOptions normalize(PoolOptions options) {
    if (options.capacity == 0) {
      throw std::invalid_argument("capacity must be greater than zero");
    }
    if (options.local_cache_capacity == 0) {
      throw std::invalid_argument("local_cache_capacity must be greater than zero");
    }
    if (options.bulk_size == 0) {
      throw std::invalid_argument("bulk_size must be greater than zero");
    }
    return options;
  }

  typename detail::LocalCacheRegistry<T>::Cache& local_cache() noexcept {
    return detail::LocalCacheRegistry<T>::cache_for(this, options_.local_cache_capacity,
                                                    &TurboMemPool::register_cache);
  }

  void refill(typename detail::LocalCacheRegistry<T>::Cache& cache) noexcept {
    std::vector<slot_type*> buffer(options_.bulk_size);
    const std::size_t count = global_.pop_bulk(buffer.data(), buffer.size());
    cache.slots.insert(cache.slots.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
  }

  void drain(typename detail::LocalCacheRegistry<T>::Cache& cache) noexcept {
    const std::size_t count = std::min(options_.bulk_size, cache.slots.size());
    if (count == 0) {
      return;
    }
    const std::size_t offset = cache.slots.size() - count;
    global_.push_bulk(cache.slots.data() + static_cast<std::ptrdiff_t>(offset), count);
    cache.slots.resize(offset);
  }

  void drain_all(typename detail::LocalCacheRegistry<T>::Cache& cache) noexcept {
    while (!cache.slots.empty()) {
      drain(cache);
    }
  }

  slot_type* slot_from_object(T* ptr) const noexcept {
    auto* raw = reinterpret_cast<std::byte*>(ptr);
    return reinterpret_cast<slot_type*>(raw - offsetof(slot_type, storage));
  }

  static void register_cache(const void* pool_ptr, typename detail::LocalCacheRegistry<T>::Cache* cache) {
    auto* pool = const_cast<TurboMemPool*>(static_cast<const TurboMemPool*>(pool_ptr));
    pool->register_cache_instance(cache);
  }

  void register_cache_instance(typename detail::LocalCacheRegistry<T>::Cache* cache) {
    std::lock_guard<std::mutex> lock(cache_registry_mutex_);
    registered_caches_.push_back(cache);
  }

  void clear_registered_caches() noexcept {
    std::lock_guard<std::mutex> lock(cache_registry_mutex_);
    for (auto* cache : registered_caches_) {
      if (cache != nullptr) {
        cache->slots.clear();
        cache->registered = false;
      }
    }
    registered_caches_.clear();
  }

  PoolOptions options_;
  std::size_t stride_;
  detail::Mapping mapping_;
  detail::TreiberStack<T> global_;
  std::vector<slot_type*> slots_;
  std::mutex cache_registry_mutex_;
  std::vector<typename detail::LocalCacheRegistry<T>::Cache*> registered_caches_;
  std::atomic<bool> constructed_{false};
};

}  // namespace turbomem
