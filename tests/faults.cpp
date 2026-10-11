#include "faults.hpp"
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <sys/uio.h>
#include <unistd.h>

namespace {
std::mutex mutex;
std::condition_variable condition;
std::deque<faults::Step> plan;
std::vector<faults::Op> trace;
bool blocked = false;
bool released = false;

std::optional<faults::Step> action(faults::Op op) {
  std::unique_lock lock(mutex);
  trace.push_back(op);
  if (plan.empty() || plan.front().op != op)
    return {};
  if (plan.front().skip-- > 0)
    return {};
  auto step = plan.front();
  plan.pop_front();
  if (step.mode == faults::Mode::Block) {
    blocked = true;
    condition.notify_all();
    condition.wait(lock, [] { return released; });
    return {};
  }
  return step;
}

bool failure(const std::optional<faults::Step> &step) {
  if (step && step->mode == faults::Mode::Error) {
    errno = step->error;
    return true;
  }
  return false;
}

std::size_t limited(std::size_t size, const std::optional<faults::Step> &step) {
  if (!step)
    return size;
  if (step->mode == faults::Mode::Zero)
    return 0;
  return std::min(size, static_cast<std::size_t>(step->amount));
}
} // namespace

namespace faults {
void reset() {
  std::lock_guard lock(mutex);
  plan.clear();
  trace.clear();
  blocked = false;
  released = true;
  condition.notify_all();
}
void set(std::vector<Step> steps) {
  std::lock_guard lock(mutex);
  plan.assign(steps.begin(), steps.end());
  trace.clear();
  blocked = false;
  released = false;
}
bool waitBlocked(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex);
  return condition.wait_for(lock, timeout, [] { return blocked; });
}
void unblock() {
  std::lock_guard lock(mutex);
  released = true;
  condition.notify_all();
}
std::vector<Op> events() {
  std::lock_guard lock(mutex);
  return trace;
}
} // namespace faults

extern "C" {
ssize_t __real_writev(int, const iovec *, int);
ssize_t __real_write(int, const void *, size_t);
ssize_t __real_pwrite(int, const void *, size_t, off_t);
ssize_t __real_pread(int, void *, size_t, off_t);
int __real_fdatasync(int);
int __real_ftruncate(int, off_t);

ssize_t __wrap_writev(int fd, const iovec *iov, int count) {
  auto step = action(faults::Op::Writev);
  if (failure(step))
    return -1;
  if (!step)
    return __real_writev(fd, iov, count);
  if (step->mode == faults::Mode::Zero)
    return 0;
  auto left = static_cast<size_t>(step->amount);
  std::vector<iovec> prefix;
  for (int i = 0; i < count && left; ++i) {
    auto size = std::min(left, iov[i].iov_len);
    if (size)
      prefix.push_back({iov[i].iov_base, size});
    left -= size;
  }
  return __real_writev(fd, prefix.data(), static_cast<int>(prefix.size()));
}
ssize_t __wrap_write(int fd, const void *data, size_t size) {
  auto step = action(faults::Op::Write);
  return failure(step) ? -1 : __real_write(fd, data, limited(size, step));
}
ssize_t __wrap_pwrite(int fd, const void *data, size_t size, off_t offset) {
  auto step = action(faults::Op::Pwrite);
  return failure(step) ? -1
                       : __real_pwrite(fd, data, limited(size, step), offset);
}
ssize_t __wrap_pread(int fd, void *data, size_t size, off_t offset) {
  auto step = action(faults::Op::Pread);
  return failure(step) ? -1
                       : __real_pread(fd, data, limited(size, step), offset);
}
int __wrap_fdatasync(int fd) {
  auto step = action(faults::Op::Sync);
  return failure(step) ? -1 : __real_fdatasync(fd);
}
int __wrap_ftruncate(int fd, off_t offset) {
  auto step = action(faults::Op::Truncate);
  return failure(step) ? -1 : __real_ftruncate(fd, offset);
}
}
