#include "heaphook/freed_pointers.hpp"
#include "heaphook/freed_pointers_map.hpp"
#include "heaphook/heaphook.hpp"
#include "heaphook/hook_types.hpp"
#include "heaphook/o1heap_cpp_container_allocator.hpp"
#include "heaphook/o1heap_wrapper.hpp"
#include "heaphook/tlsf_conte.hpp"
#include "heaphook/tlsf_conte_wrapper.hpp"

#include <sys/mman.h>

#include <cassert>

using namespace heaphook;

constexpr size_t DEFAULT_SMALL_POOL_SIZE = 1 << 22;
constexpr bool ENABLE_STATS = false;
static thread_local O1heapWrapper g_tl_o1heap(DEFAULT_SMALL_POOL_SIZE);
static thread_local FreedPointers * g_tl_freed_ptrs = nullptr;

class ThreadLocalAllocator : public GlobalAllocator
{
private:
  TLSFConteWrapper fallback_pool_;
  FreedPointersMap freed_ptrs_map_;
  size_t tl_count_ = 0;
  size_t proc_count_ = 0;
  size_t thread_safe_dealloc_count_ = 0;

  FreedPointers * get_freed_ptr_list()
  {
    const tid_t tid = gettid();
    FreedPointers * res = freed_ptrs_map_.find(tid);
    if (!res) {
      freed_ptrs_map_.push(tid, nullptr);
      res = freed_ptrs_map_.find(tid);
      res->pop();
    }
    return res;
  }

public:
  ThreadLocalAllocator()
  {
    const size_t init_pool_size = 1 << 25;
    void * ptr =
      mmap(NULL, init_pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ptr);
    fallback_pool_.set_init_pool(init_pool_size, ptr);
    fallback_pool_.set_pool_index(32768);  // set a value > NUM_SMALL_POOLS
  }

  ~ThreadLocalAllocator()
  {
#if 0
    fprintf(stderr, "Total %ld entries in freed_ptrs_map\n", freed_ptrs_map_.size());
    for(auto& [tid, freed_ptrs]: freed_ptrs_map_) {
      fprintf(stderr, "tid %d, %ld ptrs not freed\n", tid, freed_ptrs.size());
    }
#endif
    if constexpr (ENABLE_STATS) {
      int tl_frac = 0;
      if (tl_count_ + proc_count_ > 0) {
        tl_frac = (tl_count_ * 100) / (tl_count_ + proc_count_);
      }
      fprintf(
        stderr, "tl_count: %lu (%d%%), proc_count: %lu, thread_safe_dealloc_count: %lu\n",
        tl_count_, tl_frac, proc_count_, thread_safe_dealloc_count_);
    }
  }

  void * do_alloc(size_t bytes, size_t align) override
  {
    void * ptr = nullptr;
    if (align <= O1HEAP_ALIGNMENT && bytes <= g_tl_o1heap.get_max_allocation_size()) {
      ptr = g_tl_o1heap.do_alloc(bytes);
      if constexpr (ENABLE_STATS) {
        tl_count_++;
      }
    }
    if (!ptr) {
      const size_t actual_align = std::max(MIN_ADDR_ALIGNMENT, align);
      ptr = fallback_pool_.do_memalign(bytes, actual_align);
      if constexpr (ENABLE_STATS) {
        proc_count_++;
      }
    }
    return ptr;
  }

  void do_dealloc(void * ptr) override
  {
    if (ptr && o1heapIsValidPointer(ptr)) {
      do_thread_safe_dealloc(ptr);
    } else {
      fallback_pool_.do_dealloc(ptr);
    }

    // Check if there is any pointer that has transferred to another thread and is readly to be
    // freed. If any, fetch one from freed_ptr_list and free it.
    if (!g_tl_freed_ptrs) {
      g_tl_freed_ptrs = get_freed_ptr_list();
    }
    g_tl_o1heap.do_dealloc(g_tl_freed_ptrs->pop());
  }

  void * do_alloc_zeroed(size_t size) override
  {
    void * ptr = do_alloc(size, MIN_ADDR_ALIGNMENT);
    assert(ptr);
    memset(ptr, 0, size);
    return ptr;
  }

  size_t do_get_block_size(void * ptr) override
  {
    if (ptr && o1heapIsValidPointer(ptr)) {
      return g_tl_o1heap.do_get_block_size(ptr);
    } else {
      return fallback_pool_.do_get_block_size(ptr);
    }
  }

  void * do_realloc(void * ptr, size_t new_size) override
  {
    if (!ptr) {
      // When ptr is nullptr, the behavior of realloc is the same as malloc
      return do_alloc(new_size, MIN_ADDR_ALIGNMENT);
    }

    void * new_ptr = nullptr;
    if (o1heapIsValidPointer(ptr)) {
      const size_t org_size = o1heapGetBlockSize(ptr);
      if (new_size <= org_size) {
        new_ptr = ptr;
      } else {
        new_ptr = do_alloc(new_size, MIN_ADDR_ALIGNMENT);
        assert(new_ptr);
        memcpy(new_ptr, ptr, org_size);
        do_thread_safe_dealloc(ptr);
      }
    } else {
      new_ptr = fallback_pool_.do_realloc(ptr, new_size);
    }
    return new_ptr;
  }

private:
  void do_thread_safe_dealloc(void * ptr)
  {
    assert(o1heapIsValidPointer(ptr));
    if (g_tl_o1heap.owns(ptr)) {
      g_tl_o1heap.do_dealloc(ptr);
    } else {
      // Pointer ownership has been transferred; ptr is put to freed_ptrs_map_
      // and wait for its creator to recycle it.
      const auto ptr_tid = o1heapBlockGetPoolIndex(ptr);
      freed_ptrs_map_.push(ptr_tid, ptr);

      if constexpr (ENABLE_STATS) {
        thread_safe_dealloc_count_++;
      }
    }
  }
};

GlobalAllocator & GlobalAllocator::get_instance()
{
  static ThreadLocalAllocator allocator;
  return allocator;
}
