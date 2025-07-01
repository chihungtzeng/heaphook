#pragma once
#include <memory>  // For std::allocator_traits
#include "heaphook/o1heap_wrapper.hpp"

namespace heaphook {
template <typename T>
class O1HeapCppContainerAllocator {
  private:
    O1heapWrapper o1heap_;
 public:
  using value_type = T;

  O1HeapCppContainerAllocator() noexcept: o1heap_(1<<22) {}
  template <class U>
  O1HeapCppContainerAllocator(const O1HeapCppContainerAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    void* p = o1heap_.do_alloc(n * sizeof(T));
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t) noexcept { o1heap_.do_dealloc(static_cast<void*>(p)); }

  template <typename U>
  struct rebind {
    using other = O1HeapCppContainerAllocator<U>;
  };

  // Optional: equality comparison
  using is_always_equal = std::true_type;
};

template <typename T, typename U>
bool operator==(const O1HeapCppContainerAllocator<T>&, const O1HeapCppContainerAllocator<U>&) noexcept {
  return true;
}

template <typename T, typename U>
bool operator!=(const O1HeapCppContainerAllocator<T>&, const O1HeapCppContainerAllocator<U>&) noexcept {
  return false;
}
} // namespace heaphook
