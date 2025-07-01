#include "heaphook/freed_pointers.hpp"
#include "heaphook/freed_pointers_map.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>
#include <algorithm>

TEST(freed_pointers_test, test_push_pop)
{
  heaphook::FreedPointers freed_ptrs;

  for (size_t i = 0; i < 10000; i++) {
    freed_ptrs.push(reinterpret_cast<void *>(i));
  }
  for (size_t i = 0; i < 10000; i++) {
    size_t addr = reinterpret_cast<size_t>(freed_ptrs.pop());
    EXPECT_EQ(addr, 9999 - i);
  }
  // No ptr in freed_ptrs now
  void * ptr = freed_ptrs.pop();
  EXPECT_EQ(ptr, nullptr);
}

TEST(freed_pointers_test, test_push_pop_mt)
{
  constexpr size_t UPPER_BOUND = 10000;
  heaphook::FreedPointers freed_ptrs;
  auto func = [&](size_t multiple, size_t mod) {
    for(size_t i=mod; i<UPPER_BOUND; i+=multiple) {
      freed_ptrs.push(reinterpret_cast<void *>(i));
    }
  };

  std::thread t0(func, 3, 0); // push 0, 3, 6, ..., 9999
  std::thread t1(func, 3, 1); // push 1, 4, 7, ..., 9997
  std::thread t2(func, 3, 2); // push 2, 5, 8, ..., 9998

  t0.join();
  t1.join();
  t2.join();

  std::vector<size_t> vals;
  for(size_t i=0; i<UPPER_BOUND; i++)
  {
    vals.push_back(reinterpret_cast<size_t>(freed_ptrs.pop()));
  }

  std::sort(vals.begin(), vals.end());
  for(size_t i=0; i<UPPER_BOUND; i++)
  {
    EXPECT_EQ(i, vals[i]);
  }
}



TEST(freed_pointers_map_test, test_find)
{
  heaphook::FreedPointersMap fptrs_map;
  auto freed_ptrs = fptrs_map.find(33);

  EXPECT_EQ(freed_ptrs, nullptr);
}

TEST(freed_pointers_map_test, test_push_to_same_bucket)
{
  heaphook::FreedPointersMap fptrs_map;

  for (size_t addr = 1000; addr < 1024; addr++) {
    fptrs_map.push(3117, reinterpret_cast<void *>(addr));
  }
  for (size_t addr = 1024; addr < 1100; addr++) {
    fptrs_map.push(3117 + heaphook::NUM_FREED_POINTERS_MAP_BUCKETS, reinterpret_cast<void *>(addr));
  }

  auto freed_ptrs = fptrs_map.find(3117);

  EXPECT_TRUE(freed_ptrs != nullptr);
  EXPECT_EQ(reinterpret_cast<size_t>(freed_ptrs->pop()), 1023);

  auto another = fptrs_map.find(3117 + heaphook::NUM_FREED_POINTERS_MAP_BUCKETS);
  EXPECT_TRUE(another != nullptr);
  EXPECT_EQ(reinterpret_cast<size_t>(another->pop()), 1099);
}

TEST(freed_pointers_map_test, test_thread_safe)
{
  // No segmentation fault in this test
  constexpr size_t NUM_THREADS = 16;
  constexpr size_t LOWER_BOUND = 1000;
  constexpr size_t UPPER_BOUND = 2000;
  heaphook::FreedPointersMap fptrs_map;

  auto func = [&](int tid) {
    for (size_t i = LOWER_BOUND; i < UPPER_BOUND; i++) {
      fptrs_map.push(tid, reinterpret_cast<void *>(i));
    }
  };

  std::thread ts[NUM_THREADS];
  for (int tid = 0; tid < NUM_THREADS; tid++) {
    ts[tid] = std::thread(func, tid);
  }
  for (int tid = 0; tid < NUM_THREADS; tid++) {
    ts[tid].join();
  }
  for (int tid = 0; tid < NUM_THREADS; tid++) {
    auto * freed_ptrs = fptrs_map.find(tid);
    for (size_t j = UPPER_BOUND - 1; j >= LOWER_BOUND; j--) {
      EXPECT_EQ(freed_ptrs->pop(), reinterpret_cast<void *>(j));
    }
  }
}

