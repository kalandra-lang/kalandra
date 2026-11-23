#include "sensor.h"
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#ifdef SENSOR_BLOCKS
#include <Block.h>
#endif

namespace {
struct Listener {
  Reading callback;
#ifdef SENSOR_BLOCKS
  explicit Listener(Reading f) : callback(Block_copy(f)) {}
  ~Listener() { Block_release(callback); }
  int call(int n) const { return callback(n); }
#else
  int limit;
  Listener(Reading f, int value) : callback(f), limit(value) {}
  int call(int n) const { return callback(n, limit); }
#endif
};
std::mutex gate;
std::condition_variable resumed;
std::array<std::shared_ptr<Listener>, 2> slots;
std::array<bool, 2> allocated{};
std::thread worker;
bool ready = false;
int result, releases;
} // namespace
#ifdef SENSOR_BLOCKS
unsigned long long subscribe(Reading _Nonnull cb) {
  auto item = std::make_shared<Listener>(cb);
#else
unsigned long long subscribe(Reading _Nonnull cb, int limit) {
  auto item = std::make_shared<Listener>(cb, limit);
#endif
  std::lock_guard lock(gate);
  for (unsigned i = 0; i < slots.size(); ++i)
    if (!allocated[i]) {
      allocated[i] = true;
      slots[i] = item;
      return i;
    }
  std::abort();
}
void unsubscribe(unsigned long long id) {
  std::shared_ptr<Listener> owned;
  {
    std::lock_guard lock(gate);
    if (!allocated.at(id))
      std::abort();
    allocated[id] = false;
    owned.swap(slots[id]);
    ++releases;
  }
}
int deliver(int slot, int value) {
  std::shared_ptr<Listener> lease;
  {
    std::lock_guard lock(gate);
    lease = slots.at(slot);
  }
  return lease ? lease->call(value) : -1;
}
int beginDelivery(int slot) {
  std::lock_guard lock(gate);
  auto lease = slots.at(slot);
  if (!lease || worker.joinable())
    return 0;
  ready = false;
  worker = std::thread([lease, slot] {
    std::unique_lock lock(gate);
    resumed.wait(lock, [] { return ready; });
    lock.unlock();
    result = lease->call(110);
    // The native adapter can detach itself on the delivering thread. The
    // registration token remains owned until its Kalandra owner releases it.
    if (result == -1) {
      std::lock_guard guard(gate);
      slots[slot].reset();
    }
  });
  return 1;
}
int finishDelivery() {
  {
    std::lock_guard lock(gate);
    ready = true;
  }
  resumed.notify_one();
  worker.join();
  return result;
}
int openCount() {
  std::lock_guard lock(gate);
  return int(bool(slots[0])) + int(bool(slots[1]));
}
int closeCount() {
  std::lock_guard lock(gate);
  return releases;
}
