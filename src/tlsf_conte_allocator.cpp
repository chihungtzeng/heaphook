#include "heaphook/heaphook.hpp"
#include "heaphook/hook_types.hpp"
#include "heaphook/o1heap_wrapper.hpp"
#include "heaphook/tlsf_conte.hpp"
#include "heaphook/tlsf_conte_wrapper.hpp"

#include <dlfcn.h>
#include <malloc.h>

#include <cassert>

using namespace heaphook;

// Returned address must be 16-byte aligned in 64-bit systems.
// See https://github.com/mattconte/tlsf/issues/16
constexpr size_t MIN_ADDR_ALIGNMENT = sizeof(void *) * 2;
constexpr size_t MAX_BYTES_IN_SMALL_POOL = 256;
constexpr size_t NUM_SMALL_POOLS = 13;
constexpr size_t DEFAULT_SMALL_POOL_SIZE = 1 << 23;

class TLSFConteAllocator : public GlobalAllocator
{
private:
  TLSFConteWrapper tlsf_default_pool_;
  O1heapWrapper small_pools_[NUM_SMALL_POOLS];

public:
  TLSFConteAllocator()
  {
    static const malloc_type original_malloc =
      reinterpret_cast<malloc_type>(dlsym(RTLD_NEXT, "malloc"));
    static const aligned_alloc_type original_aligned_alloc =
      reinterpret_cast<aligned_alloc_type>(dlsym(RTLD_NEXT, "aligned_alloc"));

    void * ptr = nullptr;
    for (size_t i = 0; i < NUM_SMALL_POOLS; i++) {
      size_t init_pool_size = DEFAULT_SMALL_POOL_SIZE;
      ptr = original_aligned_alloc(O1HEAP_ALIGNMENT, init_pool_size);
      assert(ptr);
      small_pools_[i].set_init_pool(init_pool_size, ptr);
      small_pools_[i].set_pool_index(i);
      ptr = nullptr;
    }

    const size_t init_pool_size = 1 << 26;
    ptr = original_malloc(init_pool_size);
    assert(ptr);
    tlsf_default_pool_.set_init_pool(init_pool_size, ptr);
    tlsf_default_pool_.set_pool_index(32768);  // set a value > NUM_SMALL_POOLS
  }

  ~TLSFConteAllocator() {
  }

  void * do_alloc(size_t bytes, size_t align) override
  {
    void * ptr = nullptr;
    if (bytes <= MAX_BYTES_IN_SMALL_POOL && align <= O1HEAP_ALIGNMENT) {
      auto idx = bytes % NUM_SMALL_POOLS;
      ptr = small_pools_[idx].do_alloc(bytes);
#if 0
      if (!ptr) {
        fprintf(
          stderr, "Force to use default pool -- bytes: %lu (bit_len: %lu), align: %lu\n", bytes,
          idx, align);
      }
#endif
    }
    if (!ptr) {
      const size_t actual_align = std::max(MIN_ADDR_ALIGNMENT, align);
      ptr = tlsf_default_pool_.do_memalign(bytes, actual_align);
    }
    return ptr;
  }

  void do_dealloc(void * ptr) override
  {
    //    static const free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT,
    //    "free"));
    if (ptr && o1heapIsValidPointer(ptr)) {
      const uint32_t pool_index = o1heapBlockGetPoolIndex(ptr);
      assert(pool_index < NUM_SMALL_POOLS);
      small_pools_[pool_index].do_dealloc(ptr);
    } else {
      tlsf_default_pool_.do_dealloc(ptr);
    }
  }

  void * do_alloc_zeroed(size_t size) override
  {
    void * ptr = nullptr;
    if (size <= MAX_BYTES_IN_SMALL_POOL) {
      auto idx = size % NUM_SMALL_POOLS;
      ptr = small_pools_[idx].do_alloc(size);
    }
    if (!ptr) {
      ptr = tlsf_default_pool_.do_memalign(size, MIN_ADDR_ALIGNMENT);
    }

    if (ptr) {
      memset(ptr, 0, size);
    }
    return ptr;
  }

  size_t do_get_block_size(void * ptr) override
  {
    if (ptr && o1heapIsValidPointer(ptr)) {
      const uint32_t pool_index = o1heapBlockGetPoolIndex(ptr);
      assert(pool_index < NUM_SMALL_POOLS);
      return small_pools_[pool_index].do_get_block_size(ptr);
    } else {
      return tlsf_default_pool_.do_get_block_size(ptr);
    }
  }

  void * do_realloc(void * ptr, size_t new_size) override
  {
    // tlsf_default_pool_ handles realloc requests.
    if (!ptr) {
      return tlsf_default_pool_.do_realloc(ptr, new_size);
    }

    if (ptr && o1heapIsValidPointer(ptr)) {
      const uint32_t pool_index = o1heapBlockGetPoolIndex(ptr);
      assert(pool_index < NUM_SMALL_POOLS);
      void * new_ptr = tlsf_default_pool_.do_memalign(new_size, MIN_ADDR_ALIGNMENT);
      if (new_ptr) {
        size_t org_size = small_pools_[pool_index].do_get_block_size(ptr);
        memcpy(new_ptr, ptr, std::min(new_size, org_size));
        small_pools_[pool_index].do_dealloc(ptr);
      }
      return new_ptr;
    } else {
      return tlsf_default_pool_.do_realloc(ptr, new_size);
    }
  }
};

GlobalAllocator & GlobalAllocator::get_instance()
{
  static TLSFConteAllocator allocator;
  return allocator;
}
