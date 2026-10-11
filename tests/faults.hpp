#pragma once
#include <chrono>
#include <sys/types.h>
#include <vector>

namespace faults {
enum class Op { Writev, Write, Pwrite, Pread, Sync, Truncate };
enum class Mode { Error, Short, Zero, Block };
struct Step {
  Op op;
  int skip = 0;
  Mode mode = Mode::Error;
  ssize_t amount = 0;
  int error = 5;
};
void reset();
void set(std::vector<Step> steps);
bool waitBlocked(std::chrono::milliseconds timeout);
void unblock();
std::vector<Op> events();
} // namespace faults
