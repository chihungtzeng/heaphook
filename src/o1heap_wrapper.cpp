#include "heaphook/o1heap_wrapper.hpp"

#include "heaphook/hook_types.hpp"
#include "heaphook/o1heap.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>

namespace heaphook
{
constexpr bool LOG_ALLOCATION = false;

O1heapWrapper::O1heapWrapper() : pool_index_(gettid())
{
  pool_start_addr_ = 0;
  pool_end_addr_ = 0;
}

O1heapWrapper::O1heapWrapper(size_t init_pool_size, bool use_env_if_possible) : pool_index_(gettid())
{
  if (use_env_if_possible && getenv("THREAD_LOCAL_POOL_SIZE")) {
    init_pool_size = atol(getenv("THREAD_LOCAL_POOL_SIZE"));
  }
  void * ptr =
    mmap(NULL, init_pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(ptr);
  set_init_pool(init_pool_size, ptr);
}

O1heapWrapper::~O1heapWrapper()
{
#if 0
  // It seems that the system automatically reclaims the mmapped region when program exits.
  // Manually deallocate the region leads to segmentation fault.
  if (pool_start_addr_) {
    size_t length = pool_end_addr_ - pool_start_addr_ + 1;
    munmap(reinterpret_cast<void *>(pool_start_addr_), length);
    pool_start_addr_ = 0;
    pool_end_addr_ = 0;
  }
#endif
}

int O1heapWrapper::set_init_pool(size_t init_pool_size, void * ptr)
{
  if (ptr) {
    memset(ptr, 0, init_pool_size);
    mem_pool_ = o1heapInit(ptr, init_pool_size);
    pool_start_addr_ = reinterpret_cast<size_t>(ptr);
    pool_end_addr_ = pool_start_addr_ + init_pool_size - 1;
    max_allocation_size_ = o1heapGetMaxAllocationSize(mem_pool_);
    // fprintf(stderr, "initialize memory pool with %lu bytes\n", init_pool_size);
    return 0;
  } else {
    fprintf(stderr, "Cannot initialize memory pool!\n");
    return 1;
  }
}

void * O1heapWrapper::do_alloc(size_t bytes)
{
  void * ptr = o1heapAllocate(mem_pool_, bytes);
  o1heapBlockSetPoolIndex(ptr, pool_index_);
  return ptr;
}

void * O1heapWrapper::do_realloc(void * ptr, size_t new_size)
{
  void * new_ptr = nullptr;

  if (ptr) {
    assert(owns(ptr));
    if (new_size == 0) {
      o1heapFree(mem_pool_, ptr);
    } else {
      new_ptr = o1heapAllocate(mem_pool_, new_size);

      if (new_ptr) {
        const size_t org_size = o1heapGetBlockSize(ptr);
        const size_t len = std::min(org_size, new_size);
        memcpy(new_ptr, ptr, len);
        do_dealloc(ptr);
      }
    }
  } else {
    new_ptr = do_alloc(new_size);
  }
  return new_ptr;
}

void O1heapWrapper::do_dealloc(void * ptr)
{
  if (ptr) {
    assert(owns(ptr));
    o1heapFree(mem_pool_, ptr);
  }
}

};  // namespace heaphook
