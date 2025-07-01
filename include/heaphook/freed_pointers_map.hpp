#pragma once
/*
Implementation of freed_pointers_map. The map is in the process-level.
When a thread frees a pointer not allocated by itself, it puts the pointer in the freed_pointers_map
pointing to the correct thread that is reponsible for deallocating it. The map is a hash tabel of 32
elements. It uses thread to index the free_pointers.
*/
#include "heaphook/spinlock.hpp"
#include "heaphook/tlsf_conte_wrapper.hpp"

#include <array>
#include <list>
#include <memory>
#include <mutex>

namespace heaphook
{
constexpr size_t NUM_FREED_POINTERS_MAP_BUCKETS = 32;
constexpr size_t MASK_OF_NUM_BUCKETS = 5; // 2^5 = 32

using tid_t = int;

class _MapNode
{
public:
  tid_t tid_;
  FreedPointers* free_pointers_;
  _MapNode * next;
};

class FreedPointersMap
{
private:
  TLSFConteWrapper alloc_;
  std::array<_MapNode *, NUM_FREED_POINTERS_MAP_BUCKETS> map_nodes_;
  std::array<SpinLock, NUM_FREED_POINTERS_MAP_BUCKETS> map_nodes_locks_;

public:
  FreedPointersMap() : alloc_(1 << 13)
  {
    for (size_t i = 0; i < map_nodes_.size(); i++) {
      map_nodes_[i] = nullptr;
    }
  }

  ~FreedPointersMap() {
    for(size_t i=0; i<map_nodes_.size(); i++) {
      _MapNode *head = map_nodes_[i];
      while(head) {
        _MapNode *cur = head;
        head = head->next;
        alloc_.do_dealloc(static_cast<void*>(cur));
      }
    }
  }

  size_t capacity() { return NUM_FREED_POINTERS_MAP_BUCKETS; }

  int push(tid_t tid, void * ptr)
  {
    FreedPointers * fptrs = find(tid);
    if (fptrs) {
      // already present in map, no double insertion.
      fptrs->push(ptr);
      return 0;
    }

    _MapNode * new_node = static_cast<_MapNode *>(alloc_.do_alloc(sizeof(_MapNode)));
    new_node->tid_ = tid;
    new_node->free_pointers_ = new FreedPointers;
    new_node->free_pointers_->push(ptr);
    new_node->next = nullptr;

    const size_t idx = tid & MASK_OF_NUM_BUCKETS;
    {
      std::lock_guard<SpinLock> lk(map_nodes_locks_[idx]);
      new_node->next = map_nodes_[idx];
      map_nodes_[idx] = new_node;
    }

    return 0;
  }

  FreedPointers * find(tid_t tid)
  {
    _MapNode*  map_node = map_nodes_[tid & MASK_OF_NUM_BUCKETS];

    FreedPointers* res = nullptr;
    while (map_node && !res) {
      if (map_node->tid_ == tid) {
        res = map_node->free_pointers_;
      }
      map_node = map_node->next;
    }
    return res;
  }
};
}  // namespace heaphook
