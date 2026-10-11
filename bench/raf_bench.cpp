#include "raf/crc32c.hpp"
#include "raf/raf.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Config {
  std::string mode = "read", path, access = "sequential", cache = "hot",
              device = "nvme0n1";
  std::size_t size = 4096, batch = 256;
  std::uint64_t bytes = 256ULL << 20;
  int threads = 1, sync_ms = 1000;
  double seconds = 0.5;
  bool crc = true, mixed = false;
};
Config parse(int argc, char **argv) {
  Config c;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc)
      throw std::runtime_error("options require a value");
    std::string key = argv[i], value = argv[i + 1];
    if (key == "--mode")
      c.mode = value;
    else if (key == "--path")
      c.path = value;
    else if (key == "--size") {
      c.mixed = value == "mixed";
      if (!c.mixed)
        c.size = std::stoull(value);
    } else if (key == "--bytes")
      c.bytes = std::stoull(value);
    else if (key == "--threads")
      c.threads = std::stoi(value);
    else if (key == "--batch")
      c.batch = std::stoull(value);
    else if (key == "--sync-ms")
      c.sync_ms = std::stoi(value);
    else if (key == "--seconds")
      c.seconds = std::stod(value);
    else if (key == "--access")
      c.access = value;
    else if (key == "--cache")
      c.cache = value;
    else if (key == "--crc")
      c.crc = std::stoi(value) != 0;
    else if (key == "--device")
      c.device = value;
    else
      throw std::runtime_error("unknown option: " + key);
  }
  if ((c.mode != "crc" && c.path.empty()) || c.threads < 1 || c.threads > 64 ||
      c.batch < 1 || c.batch > 65536 || c.size < 1 || c.size > UINT32_MAX ||
      !c.bytes || c.sync_ms < 0 || c.seconds < 0 ||
      (c.mode != "crc" && c.mode != "write" && c.mode != "read") ||
      (c.access != "random" && c.access != "sequential") ||
      (c.cache != "hot" && c.cache != "cold"))
    throw std::runtime_error("invalid benchmark configuration");
  return c;
}
std::uint64_t rng(std::uint64_t &state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
std::vector<std::string> payloads(const Config &c) {
  const auto sizes =
      c.mixed ? std::vector<std::size_t>{64, 256, 4096, 65536, 1048576}
              : std::vector<std::size_t>{c.size};
  std::vector<std::string> result;
  std::uint64_t state = 1234567;
  for (auto size : sizes) {
    result.emplace_back(size, '\0');
    for (auto &byte : result.back())
      byte = static_cast<char>(rng(state));
  }
  return result;
}
double elapsed(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}
double cpuTime() {
  timespec value{};
  ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
  return static_cast<double>(value.tv_sec) +
         static_cast<double>(value.tv_nsec) * 1e-9;
}
std::uint64_t sectorsWritten(const std::string &device) {
  std::ifstream input("/sys/class/block/" + device + "/stat");
  std::uint64_t value = 0;
  for (int i = 0; i <= 6; ++i)
    if (!(input >> value))
      return 0;
  return value * 512;
}
std::pair<std::uint64_t, std::uint64_t> dirtyBytes() {
  std::ifstream input("/proc/meminfo");
  std::string line;
  std::uint64_t dirty = 0, writeback = 0;
  while (std::getline(input, line)) {
    if (line.rfind("Dirty:", 0) == 0)
      dirty = std::stoull(line.substr(6)) * 1024;
    if (line.rfind("Writeback:", 0) == 0)
      writeback = std::stoull(line.substr(10)) * 1024;
  }
  return {dirty, writeback};
}
struct Residency {
  std::uint64_t pages = 0, resident = 0;
};
Residency residency(const std::string &path, bool evict) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("open for cache control");
  struct stat st {};
  if (::fstat(fd, &st)) {
    ::close(fd);
    throw std::runtime_error("cache fstat");
  }
  if (evict) {
    auto error = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    if (error) {
      ::close(fd);
      throw std::runtime_error("cache eviction failed");
    }
  }
  const auto size = static_cast<std::size_t>(st.st_size);
  const auto pages =
      (size + static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) - 1) /
      static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  void *map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED)
    throw std::runtime_error("cache mmap");
  std::vector<unsigned char> states(pages);
  int error = ::mincore(map, size, states.data());
  ::munmap(map, size);
  if (error)
    throw std::runtime_error("mincore");
  return {pages,
          static_cast<std::uint64_t>(std::count_if(
              states.begin(), states.end(), [](auto p) { return p & 1; }))};
}
// Read every byte, including an unaligned tail, with vectorizable XOR work.
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
struct Result {
  std::uint64_t bytes = 0, records = 0, sink = 0, sync_count = 0,
                last_sync_ns = 0, total_sync_ns = 0, max_sync_ns = 0;
  double append_seconds = 0, durable_seconds = 0, seal_seconds = 0;
  std::vector<double> latency_us;
};
struct Sample {
  double second;
  std::uint64_t bytes, dirty, writeback, storage;
};
double percentile(std::vector<double> &data, double fraction) {
  if (data.empty())
    return 0;
  std::sort(data.begin(), data.end());
  return data[static_cast<std::size_t>((data.size() - 1) * fraction)];
}
void output(const Config &c, const Result &r, double seconds, double cpu,
            std::uint64_t storage, std::size_t index, double open_seconds,
            Residency cache, const std::vector<Sample> &samples) {
  auto latency = r.latency_us;
  std::cout << std::setprecision(10) << "{\"mode\":\"" << c.mode
            << "\",\"record_size\":\""
            << (c.mixed ? "mixed" : std::to_string(c.size))
            << "\",\"threads\":" << c.threads << ",\"batch\":" << c.batch
            << ",\"sync_ms\":" << c.sync_ms << ",\"access\":\"" << c.access
            << "\",\"cache\":\"" << c.cache
            << "\",\"crc\":" << (c.crc ? "true" : "false")
            << ",\"crc_backend\":\"" << raf::crc32cBackend()
            << "\",\"payload_bytes\":" << r.bytes
            << ",\"records\":" << r.records << ",\"seconds\":" << seconds
            << ",\"bytes_per_second\":" << r.bytes / seconds
            << ",\"records_per_second\":" << r.records / seconds
            << ",\"cpu_seconds\":" << cpu
            << ",\"cpu_percent\":" << 100 * cpu / seconds
            << ",\"append_seconds\":" << r.append_seconds
            << ",\"durable_seconds\":" << r.durable_seconds
            << ",\"append_bytes_per_second\":"
            << (r.append_seconds ? r.bytes / r.append_seconds : 0)
            << ",\"seal_seconds_max\":" << r.seal_seconds
            << ",\"append_call_p50_us\":" << percentile(latency, .50)
            << ",\"append_call_p99_us\":" << percentile(latency, .99)
            << ",\"latency_samples\":" << latency.size()
            << ",\"successful_syncs\":" << r.sync_count
            << ",\"last_sync_ns_max\":" << r.last_sync_ns
            << ",\"sync_total_ns\":" << r.total_sync_ns
            << ",\"sync_max_ns\":" << r.max_sync_ns
            << ",\"storage_written_bytes\":" << storage
            << ",\"index_bytes\":" << index
            << ",\"open_seconds\":" << open_seconds
            << ",\"cache_pages_before\":" << cache.pages
            << ",\"cache_resident_before\":" << cache.resident
            << ",\"sink\":" << r.sink << ",\"samples\":[";
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (i)
      std::cout << ',';
    const auto &s = samples[i];
    std::cout << '[' << s.second << ',' << s.bytes << ',' << s.dirty << ','
              << s.writeback << ',' << s.storage << ']';
  }
  std::cout << "]}\n";
}
void writeBenchmark(const Config &c) {
  auto pool = payloads(c);
  std::vector<std::unique_ptr<raf::RecordWriter>> writers;
  for (int i = 0; i < c.threads; ++i)
    writers.push_back(std::make_unique<raf::RecordWriter>(
        c.path + "." + std::to_string(i),
        raf::WriterOptions{std::chrono::milliseconds(c.sync_ms)}));
  std::vector<Result> results(static_cast<std::size_t>(c.threads));
  std::vector<std::thread> workers;
  std::barrier gate(c.threads + 1);
  std::atomic<int> finished = 0;
  std::atomic<std::uint64_t> written = 0;
  std::mutex mutex;
  std::condition_variable condition;
  std::exception_ptr failure;
  const auto storage_start = sectorsWritten(c.device);
  const auto cpu_start = cpuTime();
  const auto begin = Clock::now();
  const auto per_thread =
      (c.bytes + static_cast<std::uint64_t>(c.threads) - 1) /
      static_cast<std::uint64_t>(c.threads);
  for (int thread = 0; thread < c.threads; ++thread)
    workers.emplace_back([&, thread] {
      gate.arrive_and_wait();
      try {
        auto &result = results[static_cast<std::size_t>(thread)];
        auto &writer = *writers[static_cast<std::size_t>(thread)];
        std::vector<raf::ByteSpan> spans;
        spans.reserve(c.batch);
        std::uint64_t calls = 0;
        while (result.bytes < per_thread &&
               (!c.seconds || elapsed(begin) < c.seconds)) {
          spans.clear();
          std::uint64_t bytes = 0;
          for (std::size_t i = 0;
               i < c.batch && result.bytes + bytes < per_thread; ++i) {
            auto &payload = pool[static_cast<std::size_t>((result.records + i) %
                                                          pool.size())];
            spans.push_back(raf::asBytes(payload));
            bytes += payload.size();
          }
          const bool sample = (calls++ % 128) == 0;
          const auto start = sample ? Clock::now() : Clock::time_point{};
          if (c.batch == 1)
            writer.append(spans.front());
          else
            writer.appendBatch(spans);
          if (sample)
            result.latency_us.push_back(elapsed(start) * 1e6);
          result.records += spans.size();
          result.bytes += bytes;
          written.fetch_add(bytes, std::memory_order_relaxed);
        }
        result.append_seconds = elapsed(begin);
        const auto seal_begin = Clock::now();
        writer.seal();
        result.seal_seconds = elapsed(seal_begin);
        result.durable_seconds = elapsed(begin);
        auto status = writer.syncStatus();
        result.sync_count = status.successful_syncs;
        result.last_sync_ns = status.last_duration_ns;
        result.total_sync_ns = status.total_duration_ns;
        result.max_sync_ns = status.max_duration_ns;
      } catch (...) {
        std::lock_guard lock(mutex);
        if (!failure)
          failure = std::current_exception();
      }
      ++finished;
      condition.notify_one();
    });
  gate.arrive_and_wait();
  std::vector<Sample> samples;
  while (finished.load() < c.threads) {
    auto [dirty, writeback] = dirtyBytes();
    samples.push_back({elapsed(begin), written.load(), dirty, writeback,
                       sectorsWritten(c.device) - storage_start});
    std::unique_lock lock(mutex);
    condition.wait_for(lock, 200ms,
                       [&] { return finished.load() == c.threads; });
  }
  for (auto &worker : workers)
    worker.join();
  if (failure)
    std::rethrow_exception(failure);
  Result combined;
  for (auto &result : results) {
    combined.bytes += result.bytes;
    combined.records += result.records;
    combined.sync_count += result.sync_count;
    combined.last_sync_ns =
        std::max(combined.last_sync_ns, result.last_sync_ns);
    combined.total_sync_ns += result.total_sync_ns;
    combined.max_sync_ns = std::max(combined.max_sync_ns, result.max_sync_ns);
    combined.append_seconds =
        std::max(combined.append_seconds, result.append_seconds);
    combined.durable_seconds =
        std::max(combined.durable_seconds, result.durable_seconds);
    combined.seal_seconds =
        std::max(combined.seal_seconds, result.seal_seconds);
    combined.latency_us.insert(combined.latency_us.end(),
                               result.latency_us.begin(),
                               result.latency_us.end());
  }
  output(c, combined, combined.durable_seconds, cpuTime() - cpu_start,
         sectorsWritten(c.device) - storage_start,
         static_cast<std::size_t>(combined.records * 8), 0, {}, samples);
}
void readBenchmark(const Config &c) {
  auto cache = residency(c.path, c.cache == "cold");
  const auto open_start = Clock::now();
  raf::RecordReader reader(c.path, {c.crc});
  const auto open_seconds = elapsed(open_start);
  if (!reader.recordCount())
    throw std::runtime_error("benchmark requires nonempty archive");
  std::uint64_t warm_sink = 0;
  if (c.cache == "hot") {
    auto it = reader.iterator();
    while (it.hasNext())
      warm_sink += consume(it.next());
    cache = residency(c.path, false);
  }
  std::vector<Result> results(static_cast<std::size_t>(c.threads));
  std::vector<std::thread> workers;
  std::barrier gate(c.threads + 1);
  std::mutex mutex;
  std::exception_ptr failure;
  const auto cpu_start = cpuTime();
  const auto begin = Clock::now();
  for (int thread = 0; thread < c.threads; ++thread)
    workers.emplace_back([&, thread] {
      gate.arrive_and_wait();
      try {
        auto &result = results[static_cast<std::size_t>(thread)];
        std::uint64_t random = 987654321 + static_cast<unsigned>(thread);
        auto it = reader.iterator();
        const auto target =
            c.cache == "cold" ? reader.recordCount() : UINT64_MAX;
        while (result.records < target) {
          if (c.cache == "hot" && (result.records % 64 == 0) &&
              elapsed(begin) >= c.seconds)
            break;
          raf::RecordView view{};
          if (c.access == "random")
            view = reader.read(rng(random) % reader.recordCount());
          else {
            if (!it.hasNext())
              it = reader.iterator();
            view = it.next();
          }
          result.sink += consume(view);
          result.bytes += view.size;
          ++result.records;
        }
        result.durable_seconds = elapsed(begin);
      } catch (...) {
        std::lock_guard lock(mutex);
        if (!failure)
          failure = std::current_exception();
      }
    });
  gate.arrive_and_wait();
  for (auto &worker : workers)
    worker.join();
  if (failure)
    std::rethrow_exception(failure);
  Result combined;
  combined.sink = warm_sink;
  for (auto &r : results) {
    combined.bytes += r.bytes;
    combined.records += r.records;
    combined.sink += r.sink;
    combined.durable_seconds =
        std::max(combined.durable_seconds, r.durable_seconds);
  }
  output(c, combined, combined.durable_seconds, cpuTime() - cpu_start, 0,
         reader.indexBytes(), open_seconds, cache, {});
}
void crcBenchmark(const Config &c) {
  auto pool = payloads(c);
  Result result;
  const auto begin = Clock::now();
  const auto cpu = cpuTime();
  while (elapsed(begin) < c.seconds) {
    for (int i = 0; i < 16; ++i) {
      auto &value = pool[result.records % pool.size()];
      result.sink += raf::crc32c(raf::asBytes(value));
      result.bytes += value.size();
      ++result.records;
    }
  }
  output(c, result, elapsed(begin), cpuTime() - cpu, 0, 0, 0, {}, {});
}
} // namespace
int main(int argc, char **argv) {
  try {
    auto config = parse(argc, argv);
    if (config.mode == "write")
      writeBenchmark(config);
    else if (config.mode == "read")
      readBenchmark(config);
    else
      crcBenchmark(config);
  } catch (const std::exception &error) {
    std::cerr << "raf_bench: " << error.what() << '\n';
    return 1;
  }
}
