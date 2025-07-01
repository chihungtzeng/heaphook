#pragma once
#include "heaphook/o1heap.hpp"
#include "heaphook/spinlock.hpp"

#include <array>
#include <atomic>
#include <thread>

namespace heaphook
{

class O1heapWrapper
{
public:
  O1heapWrapper();
  O1heapWrapper(size_t init_pool_size, bool use_env_if_possible=true);
  ~O1heapWrapper();
  int set_init_pool(size_t init_pool_size, void * ptr);
  void * do_alloc(size_t bytes);
  void * do_realloc(void * ptr, size_t new_size);
  void do_dealloc(void * ptr);
  size_t do_get_block_size(void * ptr) const noexcept { return o1heapGetBlockSize(ptr); };
  int32_t get_pool_index() const noexcept { return pool_index_; };
  size_t get_max_allocation_size() const noexcept { return max_allocation_size_; };
  bool owns(void * ptr) const noexcept
  {
    const size_t addr = reinterpret_cast<size_t>(ptr);
    return bool(addr >= pool_start_addr_ && addr <= pool_end_addr_);
  };

private:
  O1HeapInstance * mem_pool_;
  size_t pool_start_addr_;
  size_t pool_end_addr_;
  size_t max_allocation_size_;
  const int32_t pool_index_;
};
};  // namespace heaphook
