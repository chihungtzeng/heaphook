// #include <dlfcn.h>
// #include <malloc.h>

#include "heaphook/heaphook.hpp"
#include "heaphook/hook_types.hpp"
#include "heaphook/tlsf_conte.hpp"
#include "heaphook/tlsf_conte_wrapper.hpp"

#include <cassert>

using namespace heaphook;

// Returned address must be 16-byte aligned in 64-bit systems.
// See https://github.com/mattconte/tlsf/issues/16
constexpr size_t MIN_ADDR_ALIGNMENT = sizeof(void *) * 2;
constexpr size_t NUM_SMALL_POOLS = 8;
constexpr size_t DEFAULT_SMALL_POOL_SIZE = 4096 * 2;
constexpr size_t MAX_BYTES_IN_SMALL_POOL = 1 << NUM_SMALL_POOLS;

static inline size_t calc_bit_len_of(size_t bytes)
{
  static_assert(sizeof(size_t) * 8 == 64, "Works for 64-bit system");
  return sizeof(size_t) * 8 - __builtin_clzll(bytes);
}

class TLSFConteAllocator : public GlobalAllocator
{
private:
  TLSFConteWrapper tlsf_default_pool_;
  TLSFConteWrapper tlsf_small_pools_[NUM_SMALL_POOLS + 1];

public:
  TLSFConteAllocator()
  : tlsf_default_pool_(1 << 24),
    tlsf_small_pools_{
      TLSFConteWrapper(),                         // 0, dummy
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 1
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 2
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 3
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 4
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 5
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 6
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 7
      TLSFConteWrapper(DEFAULT_SMALL_POOL_SIZE),  // 8
    }
  {
    for (size_t i = 0; i <= NUM_SMALL_POOLS; i++) {
      tlsf_small_pools_[i].set_allocator_index(i);
    }
    tlsf_default_pool_.set_allocator_index(255);  // set a value > NUM_SMALL_POOLS
  }

  ~TLSFConteAllocator() {}

  void * do_alloc(size_t bytes, size_t align) override
  {
    const size_t actual_align = std::max(MIN_ADDR_ALIGNMENT, align);
    if (bytes < MAX_BYTES_IN_SMALL_POOL) {
      auto idx = calc_bit_len_of(bytes);
      assert(idx <= NUM_SMALL_POOLS);
      return tlsf_small_pools_[idx].do_memalign(bytes, actual_align);
    } else {
      return tlsf_default_pool_.do_memalign(bytes, actual_align);
    }
  }

  void do_dealloc(void * ptr) override
  {
    //    static const free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT,
    //    "free"));
    const uint32_t allocator_index = tlsf_block_get_allocator_index(ptr);
    if (allocator_index <= NUM_SMALL_POOLS) {
      tlsf_small_pools_[allocator_index].do_dealloc(ptr);
    } else {
      tlsf_default_pool_.do_dealloc(ptr);
    }
  }

  void * do_alloc_zeroed(size_t size) override
  {
    void * ptr = nullptr;
    if (size < MAX_BYTES_IN_SMALL_POOL) {
      auto idx = calc_bit_len_of(size);
      ptr = tlsf_small_pools_[idx].do_memalign(size, MIN_ADDR_ALIGNMENT);
    } else {
      ptr = tlsf_default_pool_.do_memalign(size, MIN_ADDR_ALIGNMENT);
    }

    if (ptr) {
      memset(ptr, 0, size);
    }
    return ptr;
  }

  size_t do_get_block_size(void * ptr) override
  {
    const uint32_t allocator_index = tlsf_block_get_allocator_index(ptr);
    if (allocator_index <= NUM_SMALL_POOLS) {
      return tlsf_small_pools_[allocator_index].do_get_block_size(ptr);
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

    const uint32_t allocator_index = tlsf_block_get_allocator_index(ptr);
    if (allocator_index <= NUM_SMALL_POOLS) {
      void * new_ptr = tlsf_default_pool_.do_memalign(new_size, MIN_ADDR_ALIGNMENT);
      if (new_ptr) {
        size_t org_size = tlsf_small_pools_[allocator_index].do_get_block_size(ptr);
        memcpy(new_ptr, ptr, std::min(new_size, org_size));
        tlsf_small_pools_[allocator_index].do_dealloc(ptr);
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
