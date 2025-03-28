#include "heaphook/heaphook.hpp"
#include "heaphook/hook_types.hpp"

#include <dlfcn.h>
#include <malloc.h>

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace heaphook;

// This allocator stockpiles allocated memory without releasing it.
// It is used for programs that have periodic behaviors, esp., ROS2 callback functions.
// Such programs have fixed allocation/free patterns in dealing with memory.
// We don't actually need to free the allocated memory, and thus avoids minor/major page faults.
// The main features:
//  - Fewer system calls regarding memory allocation after the program bring-up stage.
//  - No lock/synchronization, as the allocator is thread_local

// This allocator will consume more main memory, roughly 1.7 times more than normal.
// We trade performance for memory consumption, which is not a major concern because
// nowadays memory is cheap.

constexpr int MAX_BIT_LEN = 64;
constexpr uint32_t MEM_METADATA_SIGNATURE = 3141592653;

// The exponential part of possibly maximum page.
// Use `cat /proc/meminfo | grep Hugepagesize` to decide max page size
constexpr uint8_t MAX_EXP_OF_ALIGNMENT = 21;  // Hugepagesize:       2048 kB

// g_use_vanilla_allocator is used to allocate internal memory for StockpileAllocator
// Otherwise we will have the self-dependency problem.
thread_local static int g_use_vanilla_allocator = 0;

// metadata is attached to each pointer to indicate its properties.
// Given a pointer ptr, its metadata is always immediately left to the pointer.
// Thus we can use pointer arithematic to access it.
typedef struct
{
  // Make this struct's size as small as possible.
  uint32_t base_ptr_offset;  // distance between base_ptr and ptr seen by the caller
  int32_t owner_tid;         // owner's thread id
  uint32_t signature;        // signature of the metadata, must be a const
  uint8_t exp_of_alignment;  // the exponential part of the alignment parameter in memalign(). If
                             // alignment = 4096=2^12, then exp_alignment is 12
  uint8_t bit_len;           // bit length of the requested malloc'ed size
  uint16_t __dummy;          // manually pad the struct, reserved for future usage
} mem_metadata_t;

static inline mem_metadata_t & get_metadata(void * ptr)
{
  uint8_t * md_ptr = static_cast<uint8_t *>(ptr) - sizeof(mem_metadata_t);
  mem_metadata_t * md = reinterpret_cast<mem_metadata_t *>(md_ptr);
  return *md;
}

static inline bool is_created_by_stockpile_allocator(void * ptr)
{
  const auto & md = get_metadata(ptr);
  return (md.bit_len < MAX_BIT_LEN) && (md.signature == MEM_METADATA_SIGNATURE) &&
         (md.exp_of_alignment <= MAX_EXP_OF_ALIGNMENT);
}

static int32_t set_metadata(
  void * ptr, uint32_t base_ptr_offset, int32_t owner_tid, uint8_t exp_of_alignment,
  uint8_t bit_len)
{
  auto & md = get_metadata(ptr);
  md.base_ptr_offset = base_ptr_offset;
  md.signature = MEM_METADATA_SIGNATURE;
  md.owner_tid = owner_tid;
  md.bit_len = bit_len;
  md.exp_of_alignment = exp_of_alignment;
  return 0;
}

static inline void * get_base_ptr(void * ptr)
{
  const auto & md = get_metadata(ptr);
  return static_cast<void *>(static_cast<uint8_t *>(ptr) - md.base_ptr_offset);
}

static inline uint16_t calc_bit_len_of(size_t bytes)
{
  return sizeof(size_t) * 8 - __builtin_clzll(bytes);
}

class VanillaAllocator : public GlobalAllocator
{
  void * do_alloc(size_t bytes, size_t align) override
  {
    static const malloc_type original_malloc =
      reinterpret_cast<malloc_type>(dlsym(RTLD_NEXT, "malloc"));
    static const memalign_type original_memalign =
      reinterpret_cast<memalign_type>(dlsym(RTLD_NEXT, "memalign"));
    if (align == 1) {
      return original_malloc(bytes);
    } else {
      return original_memalign(align, bytes);
    }
  }

  void do_dealloc(void * ptr) override
  {
    static free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT, "free"));

    // Memory allocation during thread creation/destruction may not follow g_use_vanilla_allocator.
    // That is, some pointers are created out of our control.
    // Therefore, he we use is_created_by_stockpile_allocator() as a guard rail to ensure that
    // threads are gracefully destroyed.
    if (is_created_by_stockpile_allocator(ptr)) {
      auto & md = get_metadata(ptr);
      md.signature = 0;
      original_free(get_base_ptr(ptr));
    } else {
      original_free(ptr);
    }
  }

  void * do_alloc_zeroed(size_t size) override
  {
    static const calloc_type original_calloc =
      reinterpret_cast<calloc_type>(dlsym(RTLD_NEXT, "calloc"));
    return original_calloc(size, 1);
  }

  void * do_realloc(void * ptr, size_t new_size) override
  {
    static const realloc_type original_realloc =
      reinterpret_cast<realloc_type>(dlsym(RTLD_NEXT, "realloc"));

    return original_realloc(ptr, new_size);
  }

  size_t do_get_block_size(void * ptr) override
  {
    static malloc_usable_size_type original_malloc_usable_size =
      reinterpret_cast<malloc_usable_size_type>(dlsym(RTLD_NEXT, "malloc_usable_size"));
    return original_malloc_usable_size(ptr);
  }
};

class StockpileAllocator : public GlobalAllocator
{
private:
  uint64_t total_requested_bytes_ = 0;
  uint64_t num_use_stockpiled_memory_ = 0;
  uint64_t num_malloc_call_ = 0;
  uint64_t num_memalign_call_ = 0;
  const int32_t tid_;  // thread id

  // candidate_ptrs_2d_[exponential_of_alignment][bit_len_of_requested_size]
  std::array<std::array<std::vector<void *>, MAX_BIT_LEN + 1>, MAX_EXP_OF_ALIGNMENT + 1>
    candidate_ptrs_2d_;

public:
  StockpileAllocator() : tid_(gettid())
  {
    g_use_vanilla_allocator = 1;
    candidate_ptrs_2d_[0][2].reserve(256);
    candidate_ptrs_2d_[0][3].reserve(1024);
    candidate_ptrs_2d_[0][4].reserve(1024);
    candidate_ptrs_2d_[0][5].reserve(256);
    candidate_ptrs_2d_[0][14].reserve(256);
    for (size_t i = 0; i < candidate_ptrs_2d_.size(); i++) {
      for (size_t j = 0; j < candidate_ptrs_2d_[i].size(); j++) {
        if (candidate_ptrs_2d_[i][j].capacity() == 0) {
          candidate_ptrs_2d_[i][j].reserve(128);
        }
      }
    }
    g_use_vanilla_allocator = 0;
  }

  ~StockpileAllocator()
  {
    static const free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT, "free"));

    g_use_vanilla_allocator = 1;

    if (getenv("STOCKPILE_ALLOCATOR_VERBORSE")) {
      for (size_t i = 0; i < candidate_ptrs_2d_.size(); i++) {
        for (size_t j = 0; j < candidate_ptrs_2d_[i].size(); j++) {
          if (!candidate_ptrs_2d_[i][j].empty()) {
            size_t align = 1 << i;
            size_t bytes_lb = 1 << (j - 1);
            size_t bytes_ub = (1 << j) - 1;
            std::string bytes_range =
              "[" + std::to_string(bytes_lb) + ", " + std::to_string(bytes_ub) + "]";
            std::cout << "(alignment, bytes_range) = (" << align << ", " << bytes_range
                      << "): " << candidate_ptrs_2d_[i][j].size() << std::endl;
          }
        }
      }
    }

    for (size_t i = 0; i < candidate_ptrs_2d_.size(); i++) {
      for (size_t j = 0; j < candidate_ptrs_2d_[i].size(); j++) {
        for (auto & ptr : candidate_ptrs_2d_[i][j]) {
          auto & md = get_metadata(ptr);
          assert(is_created_by_stockpile_allocator(ptr));
          if (md.owner_tid == tid_) {
            md.signature = 0;
            void * base_ptr = get_base_ptr(ptr);
            original_free(base_ptr);
          }
        }
      }
    }

    if (getenv("STOCKPILE_ALLOCATOR_VERBORSE")) {
      double percent = 0;
      if (num_malloc_call_ > 0) {
        percent = (100.0 * num_use_stockpiled_memory_) / num_malloc_call_;
      }
      std::cout << "StockpileAllocator: tid " << tid_ << ", num_malloc_call_: " << num_malloc_call_
                << ", num_memalign_call_: " << num_memalign_call_
                << ", total_requested_bytes_: " << total_requested_bytes_
                << ", num_use_stockpiled_memory_: " << num_use_stockpiled_memory_
                << ", percent: " << percent << "%" << std::endl;
    }

    // From now on, don't use StockpileAllocator by keeping the flag to 1.
    // g_use_vanilla_allocator = 0;
  }

  void * do_alloc(size_t bytes, size_t align) override
  {
    // We allocate to the next power of 2 with respect to |bytes|.
    // For example, if bytes == 6, we allocate 8 bytes.
    // The purpose is to simply the design.

    assert(align > 0);
    assert((align & (align - 1)) == 0);
    assert(bytes > 0);

    total_requested_bytes_ += bytes;

    void * ret = nullptr;
    const auto exp_of_align = calc_bit_len_of(align) - 1;
    const auto bit_len = calc_bit_len_of(bytes);

    assert(exp_of_align <= MAX_EXP_OF_ALIGNMENT);
    assert(bit_len < candidate_ptrs_2d_[exp_of_align].size());

    auto & candidate_ptrs = candidate_ptrs_2d_[exp_of_align][bit_len];

    if (!candidate_ptrs.empty()) {
      ret = candidate_ptrs.back();
      candidate_ptrs.pop_back();
      num_use_stockpiled_memory_ += 1;
    } else {
      if (align == 1) {
        ret = do_alloc_by_malloc(bit_len);
      } else {
        ret = do_alloc_by_memalign(bit_len, align);
        assert(reinterpret_cast<size_t>(ret) % align == 0);
      }
    }
    return ret;
  }

  void * do_alloc_by_malloc(uint16_t bit_len)
  {
    /*
               returned ptr, seen by the caller
               |
      +--------------------------------+
      |        |                       |
      |metadata|  size (1 << bit_len)  |
      |        |                       |
      +--------------------------------+
      ^
      |---base_ptr (returned by original_malloc, not seen by the caller)


      sizeof(metadata) = metadata.base_ptr_offset
    */
    static const malloc_type original_malloc =
      reinterpret_cast<malloc_type>(dlsym(RTLD_NEXT, "malloc"));

    num_malloc_call_ += 1;

    constexpr size_t md_len = sizeof(mem_metadata_t);
    constexpr uint8_t exp_of_alignment = 0;
    const size_t base_ptr_size = (1 << bit_len) + md_len;
    uint8_t * base_ptr = static_cast<uint8_t *>(original_malloc(base_ptr_size));
    assert(base_ptr);
    void * ret = static_cast<void *>(base_ptr + md_len);
    set_metadata(ret, md_len, tid_, exp_of_alignment, bit_len);
    return ret;
  }

  void * do_alloc_by_memalign(uint16_t bit_len, size_t align)
  {
    /*

                        returned ptr, seen by the caller
                        |
      <---  k*align --->|
      +---------------------------------------+
      |        |        |                     |
      |padding |metadata| size (1 << bit_len) |
      |        |        |                     |
      +---------------------------------------+
      ^
      |---base_ptr (returned by memalign)

      |padding| + |metadata| = k * align = metadata.base_ptr_offset,
      where k >= 1 is an integer.
    */

    static const memalign_type original_memalign =
      reinterpret_cast<memalign_type>(dlsym(RTLD_NEXT, "memalign"));
    num_memalign_call_ += 1;
    constexpr size_t md_len = sizeof(mem_metadata_t);
    size_t k = 1;
    if (align < md_len) {
      k = md_len / align;
      if (md_len % align != 0) {
        k++;
      }
    }
    const uint32_t base_ptr_offset = k * align;
    const size_t base_ptr_size = (1 << bit_len) + base_ptr_offset;
    const uint8_t exp_of_alignment = calc_bit_len_of(align) - 1;
    uint8_t * base_ptr = static_cast<uint8_t *>(original_memalign(align, base_ptr_size));
    assert(base_ptr != nullptr);
    void * ret = static_cast<void *>(base_ptr + base_ptr_offset);
    set_metadata(ret, base_ptr_offset, tid_, exp_of_alignment, bit_len);
    return ret;
  }

  void do_dealloc(void * ptr) override
  {
    static const free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT, "free"));

    auto & md = get_metadata(ptr);

    if (!is_created_by_stockpile_allocator(ptr)) {
      original_free(ptr);
    } else {
      auto & candidate_ptrs = candidate_ptrs_2d_[md.exp_of_alignment][md.bit_len];
      if (candidate_ptrs.size() < candidate_ptrs.capacity()) {
        md.owner_tid = tid_;
        candidate_ptrs.push_back(ptr);
      } else {
        // candidate list are full, free ptr anyway.
        md.signature = 0;
        void * base_ptr = get_base_ptr(ptr);
        original_free(base_ptr);
      }
    }
  }

  void * do_alloc_zeroed(size_t size) override
  {
    void * ret = do_alloc(size, 1);
    if (ret) {
      std::memset(ret, 0, size);
    }
    return ret;
  }

  size_t do_get_block_size(void * ptr) override
  {
    static const malloc_usable_size_type original_malloc_usable_size =
      reinterpret_cast<malloc_usable_size_type>(dlsym(RTLD_NEXT, "malloc_usable_size"));

    size_t ret = 0;
    if (is_created_by_stockpile_allocator(ptr)) {
      const auto & md = get_metadata(ptr);
      ret = 1 << md.bit_len;
    } else {
      ret = original_malloc_usable_size(ptr);
    }
    return ret;
  }

  void * do_realloc(void * ptr, size_t new_size) override
  {
    assert(ptr);
    assert(new_size > 0);

    assert(is_created_by_stockpile_allocator(ptr));
    auto & md = get_metadata(ptr);
    if (md.owner_tid != tid_) {
      md.owner_tid = tid_;
    }

    const auto new_bit_len = calc_bit_len_of(new_size);

    void * ret = nullptr;
    if (md.bit_len == new_bit_len) {
      // Because we let memory size be the next power of 2,
      // ptr's actual memory size is larger than the caller would assume.
      // We can just return ptr.
      ret = ptr;
    } else {
      ret = do_alloc(new_size, 1 << md.exp_of_alignment);
      assert(ret != nullptr);
      size_t cpy_len = std::min(static_cast<size_t>(1) << md.bit_len, new_size);
      memcpy(ret, ptr, cpy_len);
      do_dealloc(ptr);
    }
    return ret;
  }
};

GlobalAllocator & GlobalAllocator::get_instance()
{
  thread_local static VanillaAllocator vanilla_allocator;
  if (g_use_vanilla_allocator) {
    return vanilla_allocator;
  } else {
    thread_local static StockpileAllocator stockpile_allocator;
    return stockpile_allocator;
  }
}
