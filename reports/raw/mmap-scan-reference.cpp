#include "raf/raf.hpp"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
std::uint64_t consume(raf::RecordView view) {
  std::uint64_t sum = 0;
  std::size_t i = 0;
  for (; view.size - i >= 8; i += 8) {
    std::uint64_t word;
    std::memcpy(&word, view.data + i, 8);
    sum ^= word;
  }
  unsigned shift = 0;
  for (; i < view.size; ++i, shift += 8)
    sum ^= static_cast<std::uint64_t>(std::to_integer<unsigned>(view.data[i]))
           << shift;
  return sum;
}

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  const int threads = std::stoi(argv[2]);
  const int fd = ::open(argv[1], O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 3;
  struct stat info {};
  if (::fstat(fd, &info) != 0)
    return 4;
  const auto size = static_cast<std::size_t>(info.st_size);
  const auto *map = static_cast<const std::byte *>(
      ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
  ::close(fd);
  if (map == MAP_FAILED)
    return 5;
  constexpr std::size_t block = 1U << 20, budget = 512U << 20;
  if (size < budget)
    return 6;
  constexpr auto count = budget / block;
  std::uint64_t warm = 0;
  for (std::size_t i = 0; i < count; ++i)
    warm += consume({map + i * block, block});
  struct Result {
    std::uint64_t records = 0, sink = 0;
    double seconds = 0;
  };
  std::vector<Result> results(static_cast<std::size_t>(threads));
  std::vector<std::thread> workers;
  std::barrier gate(threads + 1);
  using Clock = std::chrono::steady_clock;
  const auto begin = Clock::now();
  for (int t = 0; t < threads; ++t)
    workers.emplace_back([&, t] {
      gate.arrive_and_wait();
      auto &r = results[static_cast<std::size_t>(t)];
      std::size_t i = 0;
      for (;;) {
        if ((r.records % 64 == 0) &&
            std::chrono::duration<double>(Clock::now() - begin).count() >= 5)
          break;
        r.sink += consume({map + i * block, block});
        if (++i == count)
          i = 0;
        ++r.records;
      }
      r.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    });
  gate.arrive_and_wait();
  for (auto &worker : workers)
    worker.join();
  std::uint64_t records = 0, sink = warm;
  double seconds = 0;
  for (const auto &r : results) {
    records += r.records;
    sink += r.sink;
    seconds = std::max(seconds, r.seconds);
  }
  std::cout.precision(12);
  std::cout << "{\"threads\":" << threads
            << ",\"payload_bytes\":" << records * block
            << ",\"seconds\":" << seconds
            << ",\"bytes_per_second\":" << records * block / seconds
            << ",\"sink\":" << sink << "}\n";
  ::munmap(const_cast<std::byte *>(map), size);
}
