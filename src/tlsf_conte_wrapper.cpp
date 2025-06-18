#include "heaphook/tlsf_conte_wrapper.hpp"

#include "heaphook/hook_types.hpp"

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

TLSFConteWrapper::TLSFConteWrapper()
{
  for (size_t i = 0; i < mmap_areas_.size(); i++) {
    mmap_areas_[i].addr = nullptr;
    mmap_areas_[i].length = 0;
  }
}

TLSFConteWrapper::~TLSFConteWrapper()
{
#if 0
  static const free_type original_free = reinterpret_cast<free_type>(dlsym(RTLD_NEXT, "free"));

  // When program exits, mmaped regions are freed automatically.
  // If we manually munmap these regions, the exit subroutine will umunmap them
  // again and thus trigger a segmentation fault.
  std::lock_guard<SpinLock> guard(spinlock_);
  for (size_t i = 0; i < num_mmap_areas_; i++) {
    if (mmap_areas_[i].addr) {
      original_free(mmap_areas_[i].addr);
      mmap_areas_[i].addr = nullptr;
      mmap_areas_[i].length = 0;
    }
  }
#endif  
}

int TLSFConteWrapper::set_init_pool(size_t init_pool_size,  void * ptr)
{
  if (ptr) {
    memset(ptr, 0, init_pool_size);
    mmap_areas_[0].addr = ptr;
    mmap_areas_[0].length = init_pool_size;
    mem_pool_ = tlsf_create_with_pool(mmap_areas_[0].addr, mmap_areas_[0].length);
    num_mmap_areas_ = 1;
    return 0;
  }
  else {
    fprintf(stderr, "Cannot set pool to nullptr\n");
    return 1;
  }
}

void * TLSFConteWrapper::do_alloc(size_t bytes)
{
  void * ptr = nullptr;

  {
    std::lock_guard<SpinLock> guard(spinlock_);
    ptr = tlsf_malloc(mem_pool_, bytes);
  }
  while ((!ptr) && num_mmap_areas_ < mmap_areas_.size() && bytes < tlsf_block_size_max()) {
    if (LOG_ALLOCATION) {
      fprintf(stderr, "[Pid %d][Tid %d] Fail to do_alloc(bytes=%lu)\n", getpid(), gettid(), bytes);
    }
    increase_mmap_area();
    {
      std::lock_guard<SpinLock> guard(spinlock_);
      ptr = tlsf_malloc(mem_pool_, bytes);
    }
  }
  if (ptr) {
    tlsf_block_set_pool_index(ptr, pool_index_);
  }
  return ptr;
}

void * TLSFConteWrapper::do_realloc(void * ptr, size_t new_size)
{
  void * new_ptr = nullptr;

  {
    std::lock_guard<SpinLock> guard(spinlock_);
    new_ptr = tlsf_realloc(mem_pool_, ptr, new_size);
  }

  while ((!new_ptr) && (new_size > 0) && num_mmap_areas_ < mmap_areas_.size() &&
         new_size < tlsf_block_size_max()) {
    if (LOG_ALLOCATION) {
      fprintf(
        stderr, "[Pid %d][Tid %d] Fail to do_realloc(ptr=%p, new_size=%lu)\n", getpid(), gettid(),
        ptr, new_size);
    }
    increase_mmap_area();
    {
      std::lock_guard<SpinLock> guard(spinlock_);
      new_ptr = tlsf_realloc(mem_pool_, ptr, new_size);
    }
  }

  if (new_ptr) {
    tlsf_block_set_pool_index(new_ptr, pool_index_);
  }
  return new_ptr;
}

void * TLSFConteWrapper::do_memalign(size_t bytes, size_t alignment)
{
  void * ptr = nullptr;
  {
    std::lock_guard<SpinLock> guard(spinlock_);
    ptr = tlsf_memalign(mem_pool_, alignment, bytes);
  }

  while ((!ptr) && (bytes > 0) && num_mmap_areas_ < mmap_areas_.size() &&
         bytes < tlsf_block_size_max()) {
    if (LOG_ALLOCATION) {
      fprintf(
        stderr, "[Pid %d][Tid %d] Fail to do_memalign(bytes=%lu, align=%lu)\n", getpid(), gettid(),
        bytes, alignment);
    }
    increase_mmap_area();
    {
      std::lock_guard<SpinLock> guard(spinlock_);
      ptr = tlsf_memalign(mem_pool_, alignment, bytes);
    }
  }

  if (ptr) {
    tlsf_block_set_pool_index(ptr, pool_index_);
  }

  return ptr;
}

void TLSFConteWrapper::do_dealloc(void * ptr)
{
  assert(owns(ptr));
  std::lock_guard<SpinLock> guard(spinlock_);
  tlsf_free(mem_pool_, ptr);
}

size_t TLSFConteWrapper::do_get_block_size(void * ptr)
{
  return tlsf_block_size(ptr);
}

bool TLSFConteWrapper::owns(void * ptr)
{
  const size_t addr = reinterpret_cast<size_t>(ptr);
  for (size_t i = 0; i < num_mmap_areas_; i++) {
    const size_t mmap_addr = reinterpret_cast<size_t>(mmap_areas_[i].addr);
    if ((addr >= mmap_addr) && (addr < mmap_addr + mmap_areas_[i].length)) {
      return true;
    }
  }
  return false;
}

int TLSFConteWrapper::set_pool_index(uint32_t index)
{
  pool_index_ = index;
  return 0;
}

int TLSFConteWrapper::increase_mmap_area()
{
  static const malloc_type original_malloc =
    reinterpret_cast<malloc_type>(dlsym(RTLD_NEXT, "malloc"));
  std::lock_guard<SpinLock> guard(spinlock_);
  const size_t length = mmap_areas_[num_mmap_areas_ - 1].length * 2;
  // void * ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void * ptr = original_malloc(length);
  if (ptr) {
    memset(ptr, 0, length);
    mmap_areas_[num_mmap_areas_].addr = ptr;
    mmap_areas_[num_mmap_areas_].length = length;
    num_mmap_areas_++;
    tlsf_add_pool(mem_pool_, ptr, length);

    if (LOG_ALLOCATION) {
      size_t total_bytes = 0;
      for (size_t i = 0; i < num_mmap_areas_; i++) {
        total_bytes += mmap_areas_[i].length;
      }
      fprintf(
        stderr,
        "[Pid %d][Tid %d] Add %lu bytes to pool %u, total allocation: %lu bytes (~= %lu MB)\n",
        getpid(), gettid(), length, pool_index_, total_bytes, total_bytes >> 20);
    }
    return 0;
  } else {
    if (LOG_ALLOCATION) {
      fprintf(
        stderr, "[Pid %d][Tid %d] Fail to add %lu bytes to pool, ptr: %p\n", getpid(), gettid(),
        length, ptr);
    }
    return 1;
  }
}
};  // namespace heaphook
