#include "heaphook/o1heap_wrapper.hpp"

#include "heaphook/hook_types.hpp"
#include "heaphook/o1heap.hpp"

#include <dlfcn.h>
#include <malloc.h>
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

O1heapWrapper::O1heapWrapper()
{
  pool_addr_ = 0;
  pool_size_ = 0;
}

O1heapWrapper::~O1heapWrapper()
{
#if 0
  static const free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT, "free"));
  std::lock_guard<SpinLock> guard(spinlock_);
  if (pool_addr_) {
    original_free(reinterpret_cast<void *>(pool_addr_));
    pool_addr_ = 0;
  }
#endif
}

int O1heapWrapper::set_init_pool(size_t init_pool_size, void* ptr)
{
/*
  static const aligned_alloc_type original_aligned_alloc =
    reinterpret_cast<aligned_alloc_type>(dlsym(RTLD_NEXT, "aligned_alloc"));

  void * ptr = original_aligned_alloc(O1HEAP_ALIGNMENT, init_pool_size);
  */
  if (ptr) {
    memset(ptr, 0, init_pool_size);
    mem_pool_ = o1heapInit(ptr, init_pool_size);
    pool_addr_ = reinterpret_cast<size_t>(ptr);
    pool_size_ = init_pool_size;
    return 0;
  } else {
    fprintf(stderr, "Cannot initialize memory pool!\n");
    return 1;
  }
}



void * O1heapWrapper::do_alloc(size_t bytes)
{
  void * ptr = nullptr;
  {
    std::lock_guard<SpinLock> guard(spinlock_);
    ptr = o1heapAllocate(mem_pool_, bytes);
    o1heapBlockSetPoolIndex(ptr, pool_index_);
  }
  return ptr;
}

void * O1heapWrapper::do_realloc(void * ptr, size_t new_size)
{
  void * new_ptr = nullptr;

  if (ptr) {
    assert(owns(ptr));
    if (new_size == 0) {
      std::lock_guard<SpinLock> guard(spinlock_);
      o1heapFree(mem_pool_, ptr);
    } else {
      {
        std::lock_guard<SpinLock> guard(spinlock_);
        new_ptr = o1heapAllocate(mem_pool_, new_size);
      }

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
    std::lock_guard<SpinLock> guard(spinlock_);
    o1heapFree(mem_pool_, ptr);
  }
}

size_t O1heapWrapper::do_get_block_size(void * ptr)
{
  return o1heapGetBlockSize(ptr);
}

bool O1heapWrapper::owns(void * ptr)
{
  const size_t addr = reinterpret_cast<size_t>(ptr);
  return bool(addr >= pool_addr_ && addr < pool_addr_ + pool_size_);
}

int O1heapWrapper::set_pool_index(uint32_t index)
{
  pool_index_ = index;
  return 0;
}

};  // namespace heaphook
