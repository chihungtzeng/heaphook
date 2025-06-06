#pragma once
#include <atomic>
#include <thread>

namespace heaphook
{
class SpinLock
{
private:
  std::atomic_flag flag = ATOMIC_FLAG_INIT;

public:
  void lock()
  {
    while (flag.test_and_set(std::memory_order_acquire)) {
      // Busy-wait
      // std::this_thread::yield(); // faster, but non-deterministic
    }
  }

  void unlock() { flag.clear(std::memory_order_release); }
};
};  // namespace heaphook
