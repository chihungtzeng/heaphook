#pragma once
#include <array>
#include <atomic>
#include <thread>

#include "heaphook/o1heap.hpp"
#include "heaphook/spinlock.hpp"

namespace heaphook {

class O1heapWrapper {
 public:
  O1heapWrapper();
  ~O1heapWrapper();
  int set_init_pool(size_t init_pool_size,  void * ptr);
  void* do_alloc(size_t bytes);
  void* do_realloc(void* ptr, size_t new_size);
  void do_dealloc(void* ptr);
  size_t do_get_block_size(void* ptr);
  int set_pool_index(uint32_t index);


 private:
  bool owns(void* ptr);

  SpinLock spinlock_;
  O1HeapInstance* mem_pool_;
  size_t pool_addr_;
  size_t pool_size_;
  uint32_t pool_index_ = 255;
};
};  // namespace heaphook
