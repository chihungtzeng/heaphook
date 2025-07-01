#pragma once
#include "heaphook/o1heap_cpp_container_allocator.hpp"
#include "heaphook/spinlock.hpp"
#include "heaphook/tlsf_conte_wrapper.hpp"

#include <list>
#include <mutex>

namespace heaphook
{
struct _ListNode
{
  void * ptr_;
  _ListNode * next_;
};

class FreedPointers
{
private:
  TLSFConteWrapper alloc_;
  SpinLock head_lock_;
  _ListNode * head_;

public:
  FreedPointers() : alloc_(1 << 13), head_(nullptr) {}

  void push(void * ptr)
  {
    _ListNode * node = static_cast<_ListNode *>(alloc_.do_alloc(sizeof(_ListNode)));
    node->ptr_ = ptr;
    node->next_ = nullptr;
    {
      std::lock_guard<SpinLock> lk(head_lock_);
      if (head_) {
        node->next_ = head_;
      }
      head_ = node;
    }
  }
  void * pop()
  {
    void * ptr = nullptr;
    {
      _ListNode * prev_head = nullptr;
      {
        std::lock_guard<SpinLock> lk(head_lock_);
        if (head_) {
          ptr = head_->ptr_;
          prev_head = head_;
          head_ = head_->next_;
        }
      }
      if (prev_head) {
        prev_head->next_ = nullptr;
        prev_head->ptr_ = nullptr;
        alloc_.do_dealloc(prev_head);
      }
    }
    return ptr;
  }
};
}  // namespace heaphook
