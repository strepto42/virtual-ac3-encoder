// Mailbox.h — lock-free hand-over of the latest value from writer threads to ONE reader thread.
//
// Three slots: one owned by the writer, one owned by the reader, one "in the middle". The writer fills
// its slot and swaps it with the middle one; the reader swaps its slot with the middle one when
// something fresh is waiting. The reader never waits for anybody and never sees a half-written value;
// if the writer posts several times between two reads, only the newest value is delivered.
// Writers are serialised among themselves by a tiny spin lock (the reader never takes it).
// No mutex, no allocation after construction.
#pragma once

#include <atomic>
#include <cstdint>

namespace vac3dsp {

template <class T>
class Mailbox
{
public:
  // Any thread.
  void Post(const T& value)
  {
    Lock lock(busy_);
    last_ = value;
    Publish(value);
  }

  // Any thread: deliver the most recently posted value to the reader once more.
  void Repost()
  {
    Lock lock(busy_);
    Publish(last_);
  }

  // The reader thread only. Returns the newest value posted since the previous call, or nullptr when
  // there is nothing new. The pointer stays valid until the next Take().
  const T* Take()
  {
    if ((mid_.load(std::memory_order_relaxed) & kFresh) == 0u)
      return nullptr;
    const uint32_t old = mid_.exchange(static_cast<uint32_t>(front_), std::memory_order_acq_rel);
    front_ = static_cast<int>(old & 3u);
    return &slots_[front_];
  }

private:
  static constexpr uint32_t kFresh = 4u;

  void Publish(const T& value)
  {
    slots_[back_] = value;
    const uint32_t old = mid_.exchange(static_cast<uint32_t>(back_) | kFresh, std::memory_order_acq_rel);
    back_ = static_cast<int>(old & 3u);
  }

  struct Lock
  {
    explicit Lock(std::atomic<bool>& f) : flag(f)
    {
      while (flag.exchange(true, std::memory_order_acquire))
      {
      }
    }
    ~Lock() { flag.store(false, std::memory_order_release); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    std::atomic<bool>& flag;
  };

  T slots_[3] = {};
  T last_ = {};
  std::atomic<uint32_t> mid_{1u};   // bits 0-1: index of the middle slot, bit 2: fresh data waiting
  int back_ = 0;                    // writer side (guarded by busy_)
  int front_ = 2;                   // reader side
  std::atomic<bool> busy_{false};
};

}  // namespace vac3dsp
