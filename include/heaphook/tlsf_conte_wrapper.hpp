#pragma once
#include <array>
#include <atomic>
#include <thread>

#include "heaphook/spinlock.hpp"
#include "heaphook/tlsf_conte.hpp"

namespace heaphook {
struct MMAPArea {
  void* addr;
  size_t length;
};

class TLSFConteWrapper {
 public:
  TLSFConteWrapper();
  TLSFConteWrapper(size_t init_pool_size);
  ~TLSFConteWrapper();
  void* do_alloc(size_t bytes);
  void* do_realloc(void* ptr, size_t new_size);
  void* do_memalign(size_t bytes, size_t alignment);
  void do_dealloc(void* ptr);
  size_t do_get_block_size(void* ptr);

  bool ptr_is_mine(void* ptr);
  int set_allocator_index(uint32_t index);

 private:
  int increase_mmap_area();

  std::array<MMAPArea, 16> mmap_areas_;
  SpinLock spinlock_;
  tlsf_t mem_pool_;
  size_t num_mmap_areas_;
  uint32_t allocator_index_ = 255;
};
};  // namespace heaphook
