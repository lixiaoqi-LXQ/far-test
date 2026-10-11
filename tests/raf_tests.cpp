#include "faults.hpp"
#include "raf/crc32c.hpp"
#include "raf/raf.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using raf::asBytes;
namespace fs = std::filesystem;

namespace {
using Test = std::pair<std::string, std::function<void()>>;
std::vector<Test> &tests() {
  static std::vector<Test> registry;
  return registry;
}
struct Register {
  Register(const char *name, std::function<void()> fn) {
    tests().emplace_back(name, std::move(fn));
  }
};
#define TEST(name)                                                             \
  void name();                                                                 \
  Register reg_##name(#name, name);                                            \
  void name()
#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value))                                                              \
      throw std::runtime_error(std::string(__FILE__) + ":" +                   \
                               std::to_string(__LINE__) + ": " #value);        \
  } while (false)

template <class F> void expect(raf::ErrorCode code, F &&fn) {
  try {
    fn();
  } catch (const raf::Error &error) {
    CHECK(error.code() == code);
    return;
  }
  throw std::runtime_error("expected RAF error");
}

struct Temp {
  fs::path root;
  Temp() {
    char pattern[] = "/tmp/raf-tests-XXXXXX";
    auto path = ::mkdtemp(pattern);
    if (!path)
      throw std::runtime_error("mkdtemp");
    root = path;
  }
  ~Temp() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  std::string file(std::string_view name = "archive.raf") const {
    return (root / name).string();
  }
};

std::vector<std::byte> load(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  return {reinterpret_cast<const std::byte *>(bytes.data()),
          reinterpret_cast<const std::byte *>(bytes.data() + bytes.size())};
}
void save(const std::string &path, const std::vector<std::byte> &data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
  CHECK(output.good());
}
void put(std::vector<std::byte> &bytes, std::size_t at, std::uint64_t value,
         int width) {
  for (int i = 0; i < width; ++i)
    bytes[at + static_cast<std::size_t>(i)] =
        std::byte((value >> (i * 8)) & 255);
}
// Independent, deliberately slow reference for fixtures, not the library CRC.
std::uint32_t reference(raf::ByteSpan bytes) {
  std::uint32_t crc = ~0U;
  for (auto byte : bytes) {
    crc ^= std::to_integer<std::uint32_t>(byte);
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
  }
  return ~crc;
}
void headerCrc(std::vector<std::byte> &bytes) {
  put(bytes, 12, reference({bytes.data(), 12}), 4);
}
std::vector<std::byte> fixture(const std::vector<std::string> &values,
                               bool sealed = true) {
  std::vector<std::byte> bytes(16);
  put(bytes, 0, 1, 4);
  std::vector<std::uint64_t> offsets;
  for (auto &value : values) {
    const auto pos = bytes.size();
    offsets.push_back(pos);
    bytes.resize(pos + value.size() + 12);
    put(bytes, pos, value.size(), 4);
    put(bytes, pos + 4, reference({bytes.data() + pos, 4}), 4);
    std::copy(asBytes(value).begin(), asBytes(value).end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(pos + 8));
    put(bytes, pos + 8 + value.size(), reference(asBytes(value)), 4);
  }
  if (sealed) {
    const auto footer = bytes.size();
    put(bytes, 4, footer, 8);
    bytes.resize(footer + offsets.size() * 8 + 4);
    for (std::size_t i = 0; i < offsets.size(); ++i)
      put(bytes, footer + i * 8, offsets[i], 8);
    put(bytes, bytes.size() - 4,
        reference({bytes.data() + footer, offsets.size() * 8}), 4);
  }
  headerCrc(bytes);
  return bytes;
}
void makeArchive(const std::string &path,
                 const std::vector<std::string> &values) {
  raf::RecordWriter writer(path, {0ms});
  for (auto &value : values)
    writer.append(asBytes(value));
  writer.seal();
}
void checkArchive(const std::string &path,
                  const std::vector<std::string> &values) {
  raf::RecordReader reader(path);
  CHECK(reader.recordCount() == values.size());
  auto iterator = reader.iterator();
  for (std::size_t i = 0; i < values.size(); ++i) {
    CHECK(reader.read(i).stringView() == values[i]);
    CHECK(iterator.hasNext());
    CHECK(iterator.next().stringView() == values[i]);
  }
  CHECK(!iterator.hasNext());
  expect(raf::ErrorCode::OutOfRange, [&] { iterator.next(); });
  expect(raf::ErrorCode::OutOfRange, [&] { reader.read(values.size()); });
  reader.validate();
}
template <class F> void parallel(int count, F fn) {
  std::exception_ptr error;
  std::mutex mutex;
  std::vector<std::thread> threads;
  for (int i = 0; i < count; ++i)
    threads.emplace_back([&, i] {
      try {
        fn(i);
      } catch (...) {
        std::lock_guard lock(mutex);
        if (!error)
          error = std::current_exception();
      }
    });
  for (auto &thread : threads)
    thread.join();
  if (error)
    std::rethrow_exception(error);
}
int child(std::string_view mode, const std::string &path) {
  auto pid = ::fork();
  CHECK(pid >= 0);
  if (!pid) {
    ::execl("/proc/self/exe", "raf_tests", "--probe", std::string(mode).c_str(),
            path.c_str(), nullptr);
    ::_exit(99);
  }
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  return WEXITSTATUS(status);
}

TEST(crc_known_vectors) {
  CHECK(raf::crc32c({}) == 0);
  CHECK(raf::crc32c(asBytes("123456789")) == 0xe3069283U);
  CHECK(raf::crc32c(asBytes("abc")) == 0x364b3fb7U);
  CHECK(raf::crc32cPortable(asBytes("123456789")) == 0xe3069283U);
}
TEST(crc_alignment_and_streaming) {
  std::string data(65545, '\0');
  for (std::size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<char>((i * 31) & 255);
  for (std::size_t offset = 0; offset < 16; ++offset) {
    auto bytes = asBytes(data).subspan(offset);
    auto want = reference(bytes);
    CHECK(raf::crc32c(bytes) == want);
    CHECK(raf::crc32cPortable(bytes) == want);
    std::uint32_t sum = 0;
    for (std::size_t at = 0; at < bytes.size(); at += 137)
      sum = raf::crc32c(
          bytes.subspan(at, std::min<std::size_t>(137, bytes.size() - at)),
          sum);
    CHECK(sum == want);
  }
}
TEST(empty_archive) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {0ms});
  CHECK(writer.recordCount() == 0);
  auto range = writer.appendBatch({});
  CHECK(range.first == 0 && range.count == 0);
  writer.close();
  writer.close();
  CHECK(load(temp.file()) == fixture({}));
  checkArchive(temp.file(), {});
  expect(raf::ErrorCode::Closed, [&] { writer.append(asBytes("x")); });
  expect(raf::ErrorCode::Closed, [&] { writer.appendBatch({}); });
  expect(raf::ErrorCode::Closed, [&] { writer.sync(); });
}
TEST(binary_variable_and_large_records) {
  Temp temp;
  std::vector<std::string> values{"", std::string("\0\xff\r\n", 4), "abc",
                                  std::string(3 * 1024 * 1024 + 7, 'z')};
  raf::RecordWriter writer(temp.file(), {0ms});
  for (std::size_t i = 0; i < values.size(); ++i)
    CHECK(writer.append(asBytes(values[i])) == i);
  writer.sync();
  CHECK(writer.syncStatus().durable_records == values.size());
  writer.seal();
  CHECK(load(temp.file()) == fixture(values));
  checkArchive(temp.file(), values);
}
TEST(reader_accepts_independent_fixture) {
  Temp temp;
  save(temp.file(), fixture({"abc", "", "defgh"}));
  checkArchive(temp.file(), {"abc", "", "defgh"});
}
TEST(batch_spans_more_than_iov_max) {
  Temp temp;
  std::vector<std::string> values;
  for (int i = 0; i < 4097; ++i)
    values.push_back(i % 9 == 0 ? "" : std::to_string(i));
  std::vector<raf::ByteSpan> spans;
  for (auto &value : values)
    spans.push_back(asBytes(value));
  raf::RecordWriter writer(temp.file(), {0ms});
  CHECK(writer.append(asBytes("first")) == 0);
  auto range = writer.appendBatch(spans);
  CHECK(range.first == 1 && range.count == values.size());
  writer.seal();
  values.insert(values.begin(), "first");
  checkArchive(temp.file(), values);
}
TEST(random_access_and_zero_copy) {
  Temp temp;
  std::vector<std::string> values;
  for (int i = 0; i < 1000; ++i)
    values.push_back(
        std::string(static_cast<std::size_t>(i % 129), static_cast<char>(i)));
  makeArchive(temp.file(), values);
  raf::RecordReader reader(temp.file());
  CHECK(reader.indexBytes() == values.size() * 8);
  std::mt19937 generator(42);
  for (int i = 0; i < 10000; ++i) {
    const auto id = generator() % values.size();
    auto a = reader.read(id);
    auto b = reader.read(id);
    CHECK(a.data == b.data);
    CHECK(a.stringView() == values[id]);
  }
  expect(raf::ErrorCode::OutOfRange, [&] { reader.read(UINT64_MAX); });
}
TEST(independent_iterators_and_readers_concurrent) {
  Temp temp;
  std::vector<std::string> values(1500, std::string(257, 'q'));
  makeArchive(temp.file(), values);
  raf::RecordReader shared(temp.file());
  parallel(8, [&](int) {
    raf::RecordReader separate(temp.file());
    for (int iteration = 0; iteration < 3; ++iteration) {
      auto a = shared.iterator();
      auto b = shared.iterator();
      std::size_t count = 0;
      while (a.hasNext()) {
        CHECK(a.next().stringView() == values[count]);
        CHECK(b.next().stringView() == values[count]);
        CHECK(separate.read(count).stringView() == values[count]);
        ++count;
      }
      CHECK(count == values.size());
    }
  });
}
TEST(one_writer_concurrent_append_is_serialized) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {5ms});
  std::vector<std::vector<std::pair<raf::RecordId, std::string>>> saved(8);
  parallel(8, [&](int thread) {
    for (int i = 0; i < 300; ++i) {
      auto value = std::to_string(thread) + ":" + std::to_string(i);
      auto id = writer.append(asBytes(value));
      saved[thread].emplace_back(id, value);
    }
  });
  writer.seal();
  raf::RecordReader reader(temp.file());
  CHECK(reader.recordCount() == 2400);
  std::vector<bool> seen(2400);
  for (auto &thread : saved)
    for (auto &[id, value] : thread) {
      CHECK(!seen[id]);
      seen[id] = true;
      CHECK(reader.read(id).stringView() == value);
    }
  reader.validate();
}
TEST(writers_on_different_files) {
  Temp temp;
  parallel(8, [&](int thread) {
    const auto path = temp.file(std::to_string(thread));
    std::vector<std::string> values(300, std::to_string(thread));
    makeArchive(path, values);
    checkArchive(path, values);
  });
}
TEST(lock_conflicts_do_not_truncate) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {0ms});
    writer.append(asBytes("keep"));
    auto before = load(temp.file());
    expect(raf::ErrorCode::Busy, [&] { raf::RecordWriter other(temp.file()); });
    expect(raf::ErrorCode::Busy,
           [&] { raf::RecordReader reader(temp.file()); });
    expect(raf::ErrorCode::Busy, [&] { raf::recover(temp.file()); });
    CHECK(load(temp.file()) == before);
    CHECK(child("reader", temp.file()) == 10);
    CHECK(child("writer", temp.file()) == 10);
    CHECK(child("recovery", temp.file()) == 10);
    writer.seal();
  }
  {
    raf::RecordReader reader(temp.file());
    raf::RecordReader other(temp.file());
    CHECK(child("reader", temp.file()) == 0);
    CHECK(child("writer", temp.file()) == 10);
    CHECK(child("recovery", temp.file()) == 10);
    expect(raf::ErrorCode::Busy,
           [&] { raf::RecordWriter writer(temp.file()); });
    expect(raf::ErrorCode::Busy, [&] { raf::recover(temp.file()); });
  }
  auto before = load(temp.file());
  expect(raf::ErrorCode::InvalidArgument,
         [&] { raf::RecordWriter writer(temp.file()); });
  CHECK(load(temp.file()) == before);
  checkArchive(temp.file(), {"keep"});
}
TEST(unsealed_file_requires_explicit_recovery) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {0ms});
    writer.append(asBytes("a"));
    writer.append(asBytes("b"));
  }
  auto before = load(temp.file());
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
  CHECK(load(temp.file()) == before);
  auto result = raf::recover(temp.file());
  CHECK(result.records == 2 && result.valid_data_end == 42);
  CHECK(result.original_file_size == 42);
  checkArchive(temp.file(), {"a", "b"});
  CHECK(raf::recover(temp.file()).records == 2);
}
TEST(process_exit_leaves_recoverable_archive) {
  Temp temp;
  CHECK(child("crash", temp.file()) == 0);
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
  CHECK(raf::recover(temp.file()).records == 2);
  checkArchive(temp.file(), {"first", "second"});
}
TEST(bad_header_crc_recovers_recognizable_version) {
  Temp temp;
  auto bytes = fixture({"a", "bb", "ccc"});
  bytes[12] ^= std::byte{1};
  save(temp.file(), bytes);
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
  CHECK(raf::recover(temp.file()).records == 3);
  checkArchive(temp.file(), {"a", "bb", "ccc"});
}
TEST(unrecognizable_header_is_not_modified) {
  Temp temp;
  for (std::size_t length = 0; length < 16; ++length) {
    std::vector<std::byte> bytes(length);
    save(temp.file(), bytes);
    expect(raf::ErrorCode::Unrecoverable, [&] { raf::recover(temp.file()); });
    CHECK(load(temp.file()) == bytes);
    expect(raf::ErrorCode::NeedsRecovery,
           [&] { raf::RecordReader reader(temp.file()); });
  }
  auto bytes = fixture({"a"});
  put(bytes, 0, 2, 4);
  headerCrc(bytes);
  save(temp.file(), bytes);
  expect(raf::ErrorCode::Unrecoverable, [&] { raf::recover(temp.file()); });
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
  CHECK(load(temp.file()) == bytes);
}
TEST(bad_footer_crc_is_rebuilt) {
  Temp temp;
  auto bytes = fixture({"a", "bb"});
  bytes.back() ^= std::byte{4};
  save(temp.file(), bytes);
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
  CHECK(raf::recover(temp.file()).records == 2);
  checkArchive(temp.file(), {"a", "bb"});
}
TEST(malicious_offsets_are_rejected) {
  Temp temp;
  for (auto bad :
       {std::uint64_t{0}, std::uint64_t{15}, std::uint64_t{18}, UINT64_MAX}) {
    auto bytes = fixture({"a", "bb"});
    put(bytes, 43, bad, 8);
    put(bytes, bytes.size() - 4, reference({bytes.data() + 43, 16}), 4);
    save(temp.file(), bytes);
    expect(raf::ErrorCode::NeedsRecovery,
           [&] { raf::RecordReader reader(temp.file()); });
    CHECK(raf::recover(temp.file()).records == 2);
  }
  auto bytes = fixture({"a", "bb"});
  put(bytes, 51, 16, 8);
  put(bytes, bytes.size() - 4, reference({bytes.data() + 43, 16}), 4);
  save(temp.file(), bytes);
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
}
TEST(malicious_footer_boundaries_are_rejected) {
  Temp temp;
  for (auto offset :
       {std::uint64_t{1}, std::uint64_t{15}, std::uint64_t{17}, UINT64_MAX}) {
    auto bytes = fixture({"a"});
    put(bytes, 4, offset, 8);
    headerCrc(bytes);
    save(temp.file(), bytes);
    expect(raf::ErrorCode::NeedsRecovery,
           [&] { raf::RecordReader reader(temp.file()); });
  }
  auto bytes = fixture({"a"});
  bytes.push_back(std::byte{0});
  save(temp.file(), bytes);
  expect(raf::ErrorCode::NeedsRecovery,
         [&] { raf::RecordReader reader(temp.file()); });
}
TEST(payload_corruption_retains_only_prefix) {
  Temp temp;
  auto bytes = fixture({"a", "bb", "ccc"});
  bytes[37] ^= std::byte{1};
  save(temp.file(), bytes);
  {
    raf::RecordReader reader(temp.file());
    CHECK(reader.read(0).stringView() == "a");
    expect(raf::ErrorCode::CorruptRecord, [&] { reader.read(1); });
    expect(raf::ErrorCode::CorruptRecord, [&] { reader.validate(); });
  }
  CHECK(raf::recover(temp.file()).records == 1);
  checkArchive(temp.file(), {"a"});
}
TEST(length_corruption_is_detected_by_full_scan) {
  Temp temp;
  for (bool repair_crc : {false, true}) {
    auto bytes = fixture({"a", "bb", "ccc"});
    put(bytes, 29, UINT32_MAX, 4);
    if (repair_crc)
      put(bytes, 33, reference({bytes.data() + 29, 4}), 4);
    save(temp.file(), bytes);
    {
      raf::RecordReader reader(temp.file());
      CHECK(reader.read(1).stringView() ==
            "bb"); // Hot path uses index-derived size.
      expect(raf::ErrorCode::CorruptRecord, [&] { reader.validate(); });
    }
    CHECK(raf::recover(temp.file()).records == 1);
    checkArchive(temp.file(), {"a"});
  }
}
TEST(crc_disabled_reader_still_full_validates) {
  Temp temp;
  auto bytes = fixture({"abc"});
  bytes[24] ^= std::byte{1};
  save(temp.file(), bytes);
  raf::RecordReader reader(temp.file(), {false});
  CHECK(reader.read(0).size == 3);
  expect(raf::ErrorCode::CorruptRecord, [&] { reader.validate(); });
}
TEST(every_truncation_position_recovers_longest_prefix) {
  Temp temp;
  const std::vector<std::string> values{"", "a", "bc", "def"};
  const auto complete = fixture(values);
  const std::vector<std::size_t> ends{28, 41, 55, 70};
  for (std::size_t cut = 0; cut <= complete.size(); ++cut) {
    auto bytes = std::vector<std::byte>(
        complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(cut));
    save(temp.file(), bytes);
    if (cut < 16) {
      expect(raf::ErrorCode::Unrecoverable, [&] { raf::recover(temp.file()); });
      CHECK(load(temp.file()) == bytes);
    } else {
      auto count = static_cast<std::size_t>(std::count_if(
          ends.begin(), ends.end(), [&](auto end) { return end <= cut; }));
      CHECK(raf::recover(temp.file()).records == count);
      checkArchive(temp.file(),
                   {values.begin(),
                    values.begin() + static_cast<std::ptrdiff_t>(count)});
    }
  }
}
TEST(recovery_large_payload_streams_and_stops_at_tail) {
  Temp temp;
  std::vector<std::string> values{std::string(8 * 1024 * 1024 + 9, 'v'), "end"};
  auto bytes = fixture(values, false);
  bytes.insert(bytes.end(), 5, std::byte{255});
  save(temp.file(), bytes);
  CHECK(raf::recover(temp.file()).records == 2);
  checkArchive(temp.file(), values);
}
TEST(writev_eintr_and_partial_iov_are_retried) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {0ms});
  faults::set({{faults::Op::Writev, 0, faults::Mode::Error, 0, EINTR},
               {faults::Op::Writev, 0, faults::Mode::Short, 2},
               {faults::Op::Writev, 0, faults::Mode::Short, 7}});
  CHECK(writer.append(asBytes("hello")) == 0);
  faults::reset();
  writer.seal();
  checkArchive(temp.file(), {"hello"});
}
TEST(short_batch_error_publishes_only_complete_records) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {0ms});
    const std::string a = "hello", b = "world", c = "tail";
    std::vector<raf::ByteSpan> spans{asBytes(a), asBytes(b), asBytes(c)};
    faults::set({{faults::Op::Writev, 0, faults::Mode::Short, 25},
                 {faults::Op::Writev, 0, faults::Mode::Error, 0, ENOSPC}});
    expect(raf::ErrorCode::Io, [&] { writer.appendBatch(spans); });
    CHECK(writer.recordCount() == 1);
    expect(raf::ErrorCode::Io, [&] { writer.append(asBytes("later")); });
    expect(raf::ErrorCode::Io, [&] { writer.seal(); });
    faults::reset();
  }
  CHECK(raf::recover(temp.file()).records == 1);
  checkArchive(temp.file(), {"hello"});
}
TEST(zero_write_is_fatal_and_recoverable) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {0ms});
    faults::set({{faults::Op::Writev, 0, faults::Mode::Zero}});
    expect(raf::ErrorCode::Io, [&] { writer.append(asBytes("x")); });
    CHECK(writer.recordCount() == 0);
    faults::reset();
  }
  CHECK(raf::recover(temp.file()).records == 0);
  checkArchive(temp.file(), {});
}
TEST(sync_watermark_and_idle_sync) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {0ms});
  faults::reset();
  writer.sync();
  CHECK(faults::events().empty());
  writer.append(asBytes("one"));
  writer.sync();
  CHECK(writer.syncStatus().durable_records == 1);
  CHECK(writer.syncStatus().successful_syncs == 1);
  CHECK(writer.syncStatus().last_duration_ns > 0);
  CHECK(writer.syncStatus().total_duration_ns ==
        writer.syncStatus().last_duration_ns);
  CHECK(writer.syncStatus().max_duration_ns ==
        writer.syncStatus().last_duration_ns);
  faults::reset();
  writer.sync();
  CHECK(faults::events().empty());
  writer.seal();
}
TEST(sync_does_not_block_append_or_overstate_watermark) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {0ms});
  writer.append(asBytes("one"));
  faults::set({{faults::Op::Sync, 0, faults::Mode::Block}});
  auto sync = std::async(std::launch::async, [&] { writer.sync(); });
  if (!faults::waitBlocked(2s)) {
    faults::unblock();
    sync.get();
    CHECK(false);
  }
  auto append = std::async(std::launch::async,
                           [&] { return writer.append(asBytes("two")); });
  bool ready = append.wait_for(1s) == std::future_status::ready;
  faults::unblock();
  sync.get();
  CHECK(append.get() == 1);
  CHECK(ready);
  CHECK(writer.syncStatus().durable_records == 1);
  faults::reset();
  writer.sync();
  CHECK(writer.syncStatus().durable_records == 2);
  writer.seal();
}
TEST(background_sync_is_dirty_only) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {10ms});
  std::this_thread::sleep_for(30ms);
  CHECK(writer.syncStatus().successful_syncs == 0);
  writer.append(asBytes("x"));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (writer.syncStatus().durable_records != 1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(2ms);
  CHECK(writer.syncStatus().durable_records == 1);
  const auto count = writer.syncStatus().successful_syncs;
  std::this_thread::sleep_for(40ms);
  CHECK(writer.syncStatus().successful_syncs == count);
  writer.seal();
}
TEST(background_sync_progresses_while_append_is_blocked) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {100ms});
  writer.append(asBytes("one"));
  faults::set({{faults::Op::Writev, 0, faults::Mode::Block}});
  auto append = std::async(std::launch::async,
                           [&] { return writer.append(asBytes("two")); });
  if (!faults::waitBlocked(2s)) {
    faults::unblock();
    append.get();
    CHECK(false);
  }
  bool synced = false;
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (!synced && std::chrono::steady_clock::now() < deadline) {
    const auto events = faults::events();
    synced = std::find(events.begin(), events.end(), faults::Op::Sync) !=
             events.end();
    if (!synced)
      std::this_thread::sleep_for(2ms);
  }
  faults::unblock();
  CHECK(append.get() == 1);
  faults::reset();
  writer.seal();
  CHECK(synced); // Completed record one must sync while record two is blocked.
}
TEST(background_sync_error_reaches_inflight_append) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {100ms});
    writer.append(asBytes("one"));
    faults::set({{faults::Op::Writev, 0, faults::Mode::Block},
                 {faults::Op::Sync, 0, faults::Mode::Error, 0, EIO}});
    auto append = std::async(std::launch::async,
                             [&] { return writer.append(asBytes("two")); });
    if (!faults::waitBlocked(2s)) {
      faults::unblock();
      append.get();
      CHECK(false);
    }
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!writer.syncStatus().last_error &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(2ms);
    const auto error = writer.syncStatus().last_error;
    faults::unblock();
    expect(raf::ErrorCode::Io, [&] { append.get(); });
    CHECK(error == EIO);
    CHECK(writer.syncStatus().durable_records == 0);
    CHECK(writer.recordCount() ==
          2); // The already-entered syscall can still finish.
    faults::reset();
  }
  CHECK(raf::recover(temp.file()).records == 2);
  checkArchive(temp.file(), {"one", "two"});
}
TEST(sync_failure_is_sticky_and_recoverable) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {0ms});
    writer.append(asBytes("one"));
    faults::set({{faults::Op::Sync, 0, faults::Mode::Error, 0, EIO}});
    expect(raf::ErrorCode::Io, [&] { writer.sync(); });
    CHECK(writer.syncStatus().durable_records == 0);
    CHECK(writer.syncStatus().last_error == EIO);
    expect(raf::ErrorCode::Io, [&] { writer.append(asBytes("two")); });
    expect(raf::ErrorCode::Io, [&] { writer.seal(); });
    faults::reset();
  }
  CHECK(raf::recover(temp.file()).records == 1);
  checkArchive(temp.file(), {"one"});
}
TEST(background_sync_failure_is_reported) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {5ms});
    faults::set({{faults::Op::Sync, 0, faults::Mode::Error, 0, EIO}});
    writer.append(asBytes("one"));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!writer.syncStatus().last_error &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(2ms);
    CHECK(writer.syncStatus().last_error == EIO);
    expect(raf::ErrorCode::Io, [&] { writer.append(asBytes("two")); });
    faults::reset();
  }
  CHECK(raf::recover(temp.file()).records == 1);
}
TEST(seal_footer_before_header_sync_order) {
  Temp temp;
  raf::RecordWriter writer(temp.file(), {0ms});
  writer.append(asBytes("x"));
  faults::reset();
  writer.seal();
  std::vector<faults::Op> ordering;
  for (auto op : faults::events())
    if (op == faults::Op::Sync || op == faults::Op::Pwrite)
      ordering.push_back(op);
  CHECK(
      (ordering == std::vector<faults::Op>{faults::Op::Sync, faults::Op::Pwrite,
                                           faults::Op::Sync}));
}
TEST(seal_failures_at_each_stage_recover) {
  Temp temp;
  std::vector<faults::Step> failures{
      {faults::Op::Write, 0, faults::Mode::Error, 0, ENOSPC},
      {faults::Op::Write, 1, faults::Mode::Error, 0, ENOSPC},
      {faults::Op::Sync, 0, faults::Mode::Error, 0, EIO},
      {faults::Op::Pwrite, 0, faults::Mode::Error, 0, EIO},
      {faults::Op::Sync, 1, faults::Mode::Error, 0, EIO}};
  for (std::size_t i = 0; i < failures.size(); ++i) {
    const auto path = temp.file(std::to_string(i));
    {
      raf::RecordWriter writer(path, {0ms});
      writer.append(asBytes("a"));
      writer.append(asBytes("bb"));
      faults::set({failures[i]});
      expect(raf::ErrorCode::Io, [&] { writer.seal(); });
      expect(raf::ErrorCode::Io, [&] { writer.append(asBytes("x")); });
      faults::reset();
    }
    CHECK(raf::recover(path).records == 2);
    checkArchive(path, {"a", "bb"});
  }
}
TEST(torn_header_and_short_footer_are_recoverable) {
  Temp temp;
  {
    raf::RecordWriter writer(temp.file(), {0ms});
    writer.append(asBytes("a"));
    faults::set({{faults::Op::Write, 0, faults::Mode::Short, 3},
                 {faults::Op::Pwrite, 0, faults::Mode::Short, 7},
                 {faults::Op::Pwrite, 0, faults::Mode::Error, 0, EIO}});
    expect(raf::ErrorCode::Io, [&] { writer.seal(); });
    faults::reset();
  }
  CHECK(raf::recover(temp.file()).records == 1);
  checkArchive(temp.file(), {"a"});
}
TEST(pread_short_and_eintr_are_retried) {
  Temp temp;
  save(temp.file(), fixture({"hello"}));
  faults::set({{faults::Op::Pread, 0, faults::Mode::Error, 0, EINTR},
               {faults::Op::Pread, 0, faults::Mode::Short, 3}});
  checkArchive(temp.file(), {"hello"});
  faults::reset();
}
TEST(reader_io_error_releases_lock) {
  Temp temp;
  save(temp.file(), fixture({"hello"}));
  faults::set({{faults::Op::Pread, 0, faults::Mode::Error, 0, EIO}});
  expect(raf::ErrorCode::Io, [&] { raf::RecordReader reader(temp.file()); });
  faults::reset();
  CHECK(raf::recover(temp.file()).records == 1);
}
TEST(recovery_io_failure_does_not_truncate_valid_file) {
  Temp temp;
  auto bytes = fixture({"hello"});
  save(temp.file(), bytes);
  faults::set({{faults::Op::Pread, 1, faults::Mode::Error, 0, EIO}});
  expect(raf::ErrorCode::Io, [&] { raf::recover(temp.file()); });
  faults::reset();
  CHECK(load(temp.file()) == bytes);
  faults::set({{faults::Op::Truncate, 0, faults::Mode::Error, 0, EIO}});
  expect(raf::ErrorCode::Io, [&] { raf::recover(temp.file()); });
  faults::reset();
  CHECK(load(temp.file()) == bytes);
  CHECK(raf::recover(temp.file()).records == 1);
}
TEST(invalid_paths_and_options) {
  Temp temp;
  expect(raf::ErrorCode::Io,
         [&] { raf::RecordReader reader(temp.file("missing")); });
  expect(raf::ErrorCode::Io, [&] { raf::recover(temp.file("missing")); });
  expect(raf::ErrorCode::Io,
         [&] { raf::RecordWriter writer(temp.file("missing/archive")); });
  expect(raf::ErrorCode::InvalidArgument,
         [&] { raf::RecordWriter writer(temp.file(), {-1ms}); });
}
TEST(recovery_mutation_failures_can_be_retried) {
  Temp temp;
  const std::vector<std::vector<faults::Step>> plans{
      {{faults::Op::Write, 0, faults::Mode::Error, 0, EIO}},
      {{faults::Op::Write, 1, faults::Mode::Error, 0, EIO}},
      {{faults::Op::Write, 0, faults::Mode::Short, 3},
       {faults::Op::Write, 0, faults::Mode::Error, 0, EIO}},
      {{faults::Op::Sync, 0, faults::Mode::Error, 0, EIO}},
      {{faults::Op::Pwrite, 0, faults::Mode::Error, 0, EIO}},
      {{faults::Op::Pwrite, 0, faults::Mode::Short, 7},
       {faults::Op::Pwrite, 0, faults::Mode::Error, 0, EIO}},
      {{faults::Op::Sync, 1, faults::Mode::Error, 0, EIO}}};
  // Persist the independent review's 14 retry experiments in the normal suite.
  for (bool sealed : {false, true})
    for (const auto &plan : plans) {
      auto bytes = fixture({"one", "two"}, sealed);
      bytes[39] ^= std::byte{1};
      save(temp.file(), bytes);
      faults::set(plan);
      expect(raf::ErrorCode::Io, [&] { raf::recover(temp.file()); });
      faults::reset();
      CHECK(raf::recover(temp.file()).records == 1);
      checkArchive(temp.file(), {"one"});
    }
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--probe") {
    try {
      auto mode = std::string_view(argv[2]);
      if (mode == "reader") {
        raf::RecordReader reader(argv[3]);
        reader.validate();
      } else if (mode == "writer") {
        raf::RecordWriter writer(argv[3], {0ms});
        writer.seal();
      } else if (mode == "recovery")
        raf::recover(argv[3]);
      else if (mode == "crash") {
        raf::RecordWriter writer(argv[3], {0ms});
        writer.append(asBytes("first"));
        writer.append(asBytes("second"));
        ::_exit(0);
      } else
        return 98;
      return 0;
    } catch (const raf::Error &error) {
      return error.code() == raf::ErrorCode::Busy ? 10 : 11;
    } catch (...) {
      return 12;
    }
  }
  std::string_view filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0;
  for (auto &[name, fn] : tests()) {
    if (!filter.empty() && name.find(filter) == std::string::npos)
      continue;
    faults::reset();
    try {
      fn();
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cout << "FAIL " << name << ": " << error.what() << '\n';
    }
    faults::reset();
  }
  std::cout << passed << " passed, " << failed << " failed\n";
  return failed || !passed ? 1 : 0;
}
