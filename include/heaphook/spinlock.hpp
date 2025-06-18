#pragma once
#include <atomic>
#include <thread>

namespace heaphook {
#if 0
class SpinLock
{
private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;

public:
  void lock()
  {
    while (flag_.test_and_set(std::memory_order_acquire)) {
      // Busy-wait
      // std::this_thread::yield(); // faster, but non-deterministic
    }
  }

  void unlock() { flag_.clear(std::memory_order_release); }
};
#else

// Code credit: https://www.youtube.com/watch?v=rmGJc9PXpuE
class SpinLock {
 private:
  std::atomic<unsigned int> flag_;

 public:
  SpinLock() : flag_(0) {}
  void lock() {
    static const timespec ns = {0, 1};
    for (int i = 0; flag_.load(std::memory_order_relaxed) ||
                    flag_.exchange(1, std::memory_order_acquire);
         i++) {
      if (i == 8) {
        i = 0;
        nanosleep(&ns, nullptr);
      }
    }
  }
  void unlock() { flag_.store(0, std::memory_order_release); }
};
#endif

};  // namespace heaphook
