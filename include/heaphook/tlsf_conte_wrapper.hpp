#pragma once
#include "heaphook/spinlock.hpp"
#include "heaphook/tlsf_conte.hpp"

#include <array>
#include <atomic>
#include <thread>

namespace heaphook
{

// Returned address must be 16-byte aligned in 64-bit systems.
constexpr size_t MIN_ADDR_ALIGNMENT = sizeof(void *) * 2;

struct MMAPArea
{
  void * addr;
  size_t length;
};

class TLSFConteWrapper
{
public:
  TLSFConteWrapper();
  TLSFConteWrapper(size_t init_pool_size);
  ~TLSFConteWrapper();
  int set_init_pool(size_t init_pool_size, void * ptr);
  void * do_alloc(size_t bytes);
  void * do_realloc(void * ptr, size_t new_size);
  void * do_memalign(size_t bytes, size_t alignment);
  void do_dealloc(void * ptr);
  size_t do_get_block_size(void * ptr);

  int set_pool_index(uint32_t index);

private:
  int increase_mmap_area();
  bool owns(void * ptr);

  std::array<MMAPArea, 12> mmap_areas_;
  SpinLock spinlock_;
  tlsf_t mem_pool_;
  size_t num_mmap_areas_;
  uint32_t pool_index_ = 255;
};
};  // namespace heaphook
