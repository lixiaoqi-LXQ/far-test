#include "raf/raf.hpp"
#include "raf/crc32c.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace raf {
Error::Error(ErrorCode code, std::string message, int system_error)
    : std::runtime_error(std::move(message)), code_(code),
      system_error_(system_error) {}
namespace {
constexpr std::uint64_t header_size = 16, record_overhead = 12;
constexpr auto max_file_size =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
using Clock = std::chrono::steady_clock;
[[noreturn]] void ioError(const char *operation, int error = errno) {
  throw Error(ErrorCode::Io,
              std::string(operation) + ": " + std::strerror(error), error);
}
[[noreturn]] void needsRecovery(const char *reason) {
  throw Error(ErrorCode::NeedsRecovery, reason);
}
[[noreturn]] void corrupt(const char *reason) {
  throw Error(ErrorCode::CorruptRecord, reason);
}
struct Fd {
  int value = -1;
  ~Fd() { reset(); }
  Fd() = default;
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  void reset() noexcept {
    if (value >= 0) {
      ::close(value);
      value = -1;
    }
  }
};
std::uint64_t fileSize(int fd) {
  struct stat info {};
  if (::fstat(fd, &info) < 0)
    ioError("fstat");
  if (!S_ISREG(info.st_mode) || info.st_size < 0)
    ioError("not a regular file", EINVAL);
  return static_cast<std::uint64_t>(info.st_size);
}
void openLocked(Fd &fd, const std::string &path, int flags, int lock) {
  fd.value = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd.value < 0)
    ioError("open");
  while (::flock(fd.value, lock | LOCK_NB) < 0) {
    if (errno == EINTR)
      continue;
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      throw Error(ErrorCode::Busy, "archive is busy", errno);
    ioError("flock");
  }
}
std::uint32_t get32(const std::byte *data) noexcept {
  return std::to_integer<std::uint32_t>(data[0]) |
         (std::to_integer<std::uint32_t>(data[1]) << 8) |
         (std::to_integer<std::uint32_t>(data[2]) << 16) |
         (std::to_integer<std::uint32_t>(data[3]) << 24);
}
std::uint64_t get64(const std::byte *data) noexcept {
  return get32(data) | (static_cast<std::uint64_t>(get32(data + 4)) << 32);
}
void put32(std::byte *data, std::uint32_t value) noexcept {
  for (unsigned i = 0; i < 4; ++i)
    data[i] = std::byte((value >> (i * 8)) & 255);
}
void put64(std::byte *data, std::uint64_t value) noexcept {
  put32(data, static_cast<std::uint32_t>(value));
  put32(data + 4, static_cast<std::uint32_t>(value >> 32));
}
std::array<std::byte, 16> makeHeader(std::uint64_t footer) noexcept {
  std::array<std::byte, 16> bytes{};
  put32(bytes.data(), 1);
  put64(bytes.data() + 4, footer);
  put32(bytes.data() + 12, crc32c({bytes.data(), 12}));
  return bytes;
}
bool validHeader(const std::array<std::byte, 16> &bytes) noexcept {
  return get32(bytes.data()) == 1 &&
         get32(bytes.data() + 12) == crc32c({bytes.data(), 12});
}
// EOF stops recovery; actual read errors abort it without truncating data.
bool readAt(int fd, std::byte *data, std::size_t size, std::uint64_t offset) {
  while (size) {
    const auto count = ::pread(fd, data, std::min<std::size_t>(size, 1U << 30),
                               static_cast<off_t>(offset));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      ioError("pread");
    }
    if (!count)
      return false;
    auto done = static_cast<std::size_t>(count);
    data += done;
    size -= done;
    offset += done;
  }
  return true;
}
void writeAll(int fd, ByteSpan bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(fd, bytes.data(),
                               std::min<std::size_t>(bytes.size(), 1U << 30));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      ioError("write");
    }
    if (!count)
      ioError("zero write", EIO);
    bytes = bytes.subspan(static_cast<std::size_t>(count));
  }
}
void writeAt(int fd, ByteSpan bytes, std::uint64_t offset) {
  while (!bytes.empty()) {
    const auto count =
        ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      ioError("pwrite");
    }
    if (!count)
      ioError("zero pwrite", EIO);
    auto done = static_cast<std::size_t>(count);
    bytes = bytes.subspan(done);
    offset += done;
  }
}
void syncFd(int fd) {
  while (::fdatasync(fd) < 0) {
    if (errno == EINTR)
      continue;
    ioError("fdatasync");
  }
}
void seekTo(int fd, std::uint64_t offset) {
  if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
    ioError("lseek");
}
void writeFooter(int fd, const std::vector<std::uint64_t> &offsets) {
  std::uint32_t crc = 0;
  if constexpr (std::endian::native == std::endian::little) {
    auto bytes = ByteSpan{reinterpret_cast<const std::byte *>(offsets.data()),
                          offsets.size() * 8};
    crc = crc32c(bytes);
    writeAll(fd, bytes);
  } else {
    std::array<std::byte, 8192> buffer;
    for (std::size_t i = 0; i < offsets.size();) {
      const auto count =
          std::min<std::size_t>(buffer.size() / 8, offsets.size() - i);
      for (std::size_t j = 0; j < count; ++j)
        put64(buffer.data() + j * 8, offsets[i + j]);
      auto bytes = ByteSpan{buffer.data(), count * 8};
      crc = crc32c(bytes, crc);
      writeAll(fd, bytes);
      i += count;
    }
  }
  std::array<std::byte, 4> checksum;
  put32(checksum.data(), crc);
  writeAll(fd, checksum);
}
std::uint64_t elapsedNs(Clock::time_point begin) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
          .count());
}
} // namespace

struct RecordWriter::State {
  Fd fd;
  std::uint64_t position = header_size;
  std::vector<std::uint64_t> offsets;
  std::atomic<std::uint64_t> completed{0};
  std::mutex mutex;      // Append and lifecycle transitions.
  std::mutex sync_mutex; // Sync and its statistics; append does not acquire it.
  std::condition_variable wake;
  std::thread background;
  std::chrono::milliseconds interval;
  SyncStatus status;
  enum class Phase { Open, Sealing, Sealed } phase = Phase::Open;
  std::atomic<int> failure{0};
  std::atomic<bool> stopping{false};
  explicit State(std::chrono::milliseconds sync_interval)
      : interval(sync_interval) {}
  ~State() { stopBackground(); }
  void requireOpen() const {
    if (const auto error = failure.load())
      ioError("writer failed; recovery required", error);
    if (phase != Phase::Open)
      throw Error(ErrorCode::Closed, "writer is closed");
  }
  void fail(int error) {
    int expected = 0;
    failure.compare_exchange_strong(expected, error ? error : EIO);
    stopping = true;
    wake.notify_all();
  }
  void stopBackground() {
    {
      std::lock_guard lock(sync_mutex);
      stopping = true;
    }
    wake.notify_all();
    if (background.joinable())
      background.join();
  }
  void noteSync(std::uint64_t duration) {
    status.last_duration_ns = duration;
    status.total_duration_ns += duration;
    status.max_duration_ns = std::max(status.max_duration_ns, duration);
  }
  void performSync(std::uint64_t target) {
    const auto begin = Clock::now();
    try {
      syncFd(fd.value);
    } catch (const Error &error) {
      noteSync(elapsedNs(begin));
      fail(error.systemError());
      throw;
    }
    status.durable_records = target;
    ++status.successful_syncs;
    noteSync(elapsedNs(begin));
  }
  void runSync(bool explicit_call) {
    std::lock_guard sync_lock(sync_mutex);
    if (explicit_call) {
      std::lock_guard lock(mutex);
      requireOpen();
    } else if (stopping.load() || failure.load())
      return;
    // A long/busy append may own mutex. Publish complete records independently
    // so the periodic sync can still take a conservative watermark.
    const auto target = completed.load(std::memory_order_acquire);
    if (target <= status.durable_records)
      return;
    performSync(target);
  }
  void startBackground() {
    if (!interval.count())
      return;
    background = std::thread([this] {
      std::unique_lock lock(sync_mutex);
      while (
          !wake.wait_for(lock, interval, [this] { return stopping.load(); })) {
        lock.unlock();
        try {
          runSync(false);
        } catch (const Error &) {
          return;
        }
        lock.lock();
      }
    });
  }
};
RecordWriter::RecordWriter(const std::string &filepath, WriterOptions options) {
  if (options.sync_interval.count() < 0)
    throw Error(ErrorCode::InvalidArgument, "negative sync interval");
  state_ = std::make_unique<State>(options.sync_interval);
  openLocked(state_->fd, filepath, O_RDWR | O_CREAT, LOCK_EX);
  if (fileSize(state_->fd.value) != 0)
    throw Error(ErrorCode::InvalidArgument,
                "writer requires a new or empty file");
  const auto header = makeHeader(0);
  writeAt(state_->fd.value, header, 0);
  seekTo(state_->fd.value, header_size);
  state_->startBackground();
}
RecordWriter::~RecordWriter() = default;
RecordId RecordWriter::append(ByteSpan payload) {
  return appendBatch({&payload, 1}).first;
}
RecordRange RecordWriter::appendBatch(Span<const ByteSpan> records) {
  auto &state = *state_;
  std::lock_guard lock(state.mutex);
  state.requireOpen();
  const RecordRange result{state.offsets.size(), records.size()};
  if (records.empty())
    return result;
  auto projected = state.position;
  for (auto payload : records) {
    if (payload.size() > UINT32_MAX)
      throw Error(ErrorCode::InvalidArgument, "payload exceeds uint32 length");
    if (payload.size() + record_overhead > max_file_size - projected)
      throw Error(ErrorCode::InvalidArgument, "archive exceeds off_t");
    projected += payload.size() + record_overhead;
  }
  const auto total_records = state.offsets.size() + records.size();
  if (total_records < state.offsets.size() || projected > max_file_size - 4 ||
      total_records > (max_file_size - projected - 4) / 8)
    throw Error(ErrorCode::InvalidArgument, "footer exceeds off_t");
  if (total_records > state.offsets.capacity())
    state.offsets.reserve(
        std::max(total_records,
                 std::max<std::size_t>(256, state.offsets.capacity() * 2)));
  constexpr std::size_t chunk_limit = 256;
  struct Metadata {
    std::array<std::byte, 8> header;
    std::array<std::byte, 4> crc;
  };
  std::array<Metadata, chunk_limit> metadata;
  std::array<iovec, chunk_limit * 3> vectors;
  std::array<std::uint64_t, chunk_limit> starts, ends;
  static const std::size_t iov_limit = [] {
    const auto value = ::sysconf(_SC_IOV_MAX);
    return static_cast<std::size_t>(value > 0 ? value : 1024);
  }();
  const auto chunk_records =
      std::min(chunk_limit, std::max<std::size_t>(1, iov_limit / 3));
  try {
    for (std::size_t base = 0; base < records.size();) {
      const auto count = std::min(chunk_records, records.size() - base);
      auto end = state.position;
      std::size_t iov_count = 0;
      for (std::size_t i = 0; i < count; ++i) {
        auto payload = records[base + i];
        put32(metadata[i].header.data(),
              static_cast<std::uint32_t>(payload.size()));
        put32(metadata[i].header.data() + 4,
              crc32c({metadata[i].header.data(), 4}));
        put32(metadata[i].crc.data(), crc32c(payload));
        starts[i] = end;
        end += payload.size() + record_overhead;
        ends[i] = end;
        vectors[iov_count++] = {metadata[i].header.data(),
                                metadata[i].header.size()};
        if (!payload.empty())
          vectors[iov_count++] = {const_cast<std::byte *>(payload.data()),
                                  payload.size()};
        vectors[iov_count++] = {metadata[i].crc.data(), metadata[i].crc.size()};
      }
      std::size_t first_iov = 0, completed = 0;
      while (first_iov < iov_count) {
        if (const auto error = state.failure.load())
          ioError("writer failed; recovery required", error);
        const auto written =
            ::writev(state.fd.value, vectors.data() + first_iov,
                     static_cast<int>(iov_count - first_iov));
        if (written < 0) {
          if (errno == EINTR)
            continue;
          ioError("writev");
        }
        if (!written)
          ioError("zero writev", EIO);
        state.position += static_cast<std::uint64_t>(written);
        while (completed < count && ends[completed] <= state.position)
          state.offsets.push_back(starts[completed++]);
        state.completed.store(state.offsets.size(), std::memory_order_release);
        auto left = static_cast<std::size_t>(written);
        while (left && first_iov < iov_count) {
          auto &vector = vectors[first_iov];
          if (left >= vector.iov_len) {
            left -= vector.iov_len;
            ++first_iov;
          } else {
            vector.iov_base = static_cast<std::byte *>(vector.iov_base) + left;
            vector.iov_len -= left;
            left = 0;
          }
        }
      }
      base += count;
    }
  } catch (const Error &error) {
    state.fail(error.systemError());
    throw;
  }
  if (const auto error = state.failure.load())
    ioError("writer failed; recovery required", error);
  return result;
}
void RecordWriter::sync() { state_->runSync(true); }
void RecordWriter::seal() {
  auto &state = *state_;
  {
    std::lock_guard lock(state.mutex);
    if (const auto error = state.failure.load())
      ioError("writer failed; recovery required", error);
    if (state.phase == State::Phase::Sealed)
      return;
    state.requireOpen();
    state.phase = State::Phase::Sealing;
  }
  state.stopBackground();
  std::lock_guard sync_lock(state.sync_mutex);
  try {
    if (const auto error = state.failure.load())
      ioError("writer failed; recovery required", error);
    writeFooter(state.fd.value, state.offsets);
    state.performSync(state.offsets.size());
    const auto header = makeHeader(state.position);
    writeAt(state.fd.value, header, 0);
    state.performSync(state.offsets.size());
    std::lock_guard lock(state.mutex);
    state.phase = State::Phase::Sealed;
    state.fd.reset();
  } catch (const Error &error) {
    std::lock_guard lock(state.mutex);
    state.fail(error.systemError());
    throw;
  }
}
std::uint64_t RecordWriter::recordCount() const {
  return state_->completed.load(std::memory_order_acquire);
}
SyncStatus RecordWriter::syncStatus() const {
  std::lock_guard lock(state_->sync_mutex);
  auto result = state_->status;
  result.last_error = state_->failure.load();
  return result;
}

struct RecordReader::State {
  Fd fd;
  std::vector<std::uint64_t> offsets;
  std::uint64_t footer = 0;
  std::size_t size = 0;
  const std::byte *mapping = nullptr;
  bool verify_crc = true;
  ~State() {
    if (mapping)
      ::munmap(const_cast<std::byte *>(mapping), size);
  }
};
RecordReader::RecordReader(const std::string &filepath, ReaderOptions options)
    : state_(std::make_unique<State>()) {
  auto &state = *state_;
  openLocked(state.fd, filepath, O_RDONLY, LOCK_SH);
  const auto size = fileSize(state.fd.value);
  if (size < header_size + 4 || size > std::numeric_limits<std::size_t>::max())
    needsRecovery("invalid archive size");
  std::array<std::byte, 16> header;
  if (!readAt(state.fd.value, header.data(), header.size(), 0) ||
      !validHeader(header))
    needsRecovery("invalid header version or CRC");
  state.footer = get64(header.data() + 4);
  if (state.footer < header_size || state.footer > size - 4)
    needsRecovery("archive is unsealed or footer is out of range");
  const auto index_bytes = size - state.footer - 4;
  if (index_bytes % 8)
    needsRecovery("invalid footer size");
  const auto count = index_bytes / 8;
  if (count > (state.footer - header_size) / record_overhead ||
      count > state.offsets.max_size())
    needsRecovery("impossible record count");
  state.offsets.resize(static_cast<std::size_t>(count));
  auto *index_data = reinterpret_cast<std::byte *>(state.offsets.data());
  if (!readAt(state.fd.value, index_data, static_cast<std::size_t>(index_bytes),
              state.footer))
    needsRecovery("truncated index");
  std::array<std::byte, 4> checksum;
  if (!readAt(state.fd.value, checksum.data(), checksum.size(), size - 4) ||
      crc32c({index_data, static_cast<std::size_t>(index_bytes)}) !=
          get32(checksum.data()))
    needsRecovery("footer CRC mismatch");
  if constexpr (std::endian::native != std::endian::little)
    for (auto &offset : state.offsets)
      offset = get64(reinterpret_cast<const std::byte *>(&offset));
  if (state.offsets.empty()) {
    if (state.footer != header_size)
      needsRecovery("empty index with record data");
  } else {
    if (state.offsets.front() != header_size)
      needsRecovery("first record does not begin after header");
    for (std::size_t i = 0; i < state.offsets.size(); ++i) {
      auto begin = state.offsets[i];
      auto end =
          i + 1 < state.offsets.size() ? state.offsets[i + 1] : state.footer;
      if (begin < header_size || end > state.footer || end <= begin ||
          end - begin < record_overhead ||
          end - begin - record_overhead > UINT32_MAX)
        needsRecovery("invalid record offsets");
    }
  }
  state.size = static_cast<std::size_t>(size);
  state.verify_crc = options.verify_payload_crc;
  auto *mapping =
      ::mmap(nullptr, state.size, PROT_READ, MAP_SHARED, state.fd.value, 0);
  if (mapping == MAP_FAILED)
    ioError("mmap");
  state.mapping = static_cast<const std::byte *>(mapping);
}
RecordReader::~RecordReader() = default;
std::uint64_t RecordReader::recordCount() const noexcept {
  return state_->offsets.size();
}
std::size_t RecordReader::indexBytes() const noexcept {
  return state_->offsets.size() * 8;
}
RecordView RecordReader::read(RecordId id) const {
  const auto &state = *state_;
  if (id >= state.offsets.size())
    throw Error(ErrorCode::OutOfRange, "record ID is out of range");
  const auto begin = state.offsets[static_cast<std::size_t>(id)];
  const auto end = id + 1 < state.offsets.size()
                       ? state.offsets[static_cast<std::size_t>(id + 1)]
                       : state.footer;
  const RecordView view{
      state.mapping + begin + 8,
      static_cast<std::size_t>(end - begin - record_overhead)};
  if (state.verify_crc &&
      crc32c(view.bytes()) != get32(state.mapping + end - 4))
    corrupt("payload CRC mismatch");
  return view;
}
void RecordReader::validate() const {
  const auto &state = *state_;
  for (std::size_t i = 0; i < state.offsets.size(); ++i) {
    auto begin = state.offsets[i];
    auto end =
        i + 1 < state.offsets.size() ? state.offsets[i + 1] : state.footer;
    auto *data = state.mapping + begin;
    if (get32(data + 4) != crc32c({data, 4}) ||
        get32(data) != end - begin - record_overhead)
      corrupt("length or length CRC mismatch");
    if (crc32c({data + 8,
                static_cast<std::size_t>(end - begin - record_overhead)}) !=
        get32(state.mapping + end - 4))
      corrupt("payload CRC mismatch");
  }
}
bool RecordReader::Iterator::hasNext() const noexcept {
  return next_ < reader_->recordCount();
}
RecordView RecordReader::Iterator::next() {
  auto view = reader_->read(next_);
  ++next_;
  return view;
}

RecoveryResult recover(const std::string &filepath) {
  Fd fd;
  openLocked(fd, filepath, O_RDWR, LOCK_EX);
  const auto original_size = fileSize(fd.value);
  std::array<std::byte, 16> header;
  if (original_size < header_size ||
      !readAt(fd.value, header.data(), header.size(), 0) ||
      get32(header.data()) != 1)
    throw Error(ErrorCode::Unrecoverable, "unrecognizable RAF header");
  auto boundary = original_size;
  if (validHeader(header)) {
    const auto footer = get64(header.data() + 4);
    if (footer >= header_size)
      boundary = std::min(boundary, footer);
  }
  std::vector<std::uint64_t> offsets;
  std::array<std::byte, 64 * 1024> buffer;
  auto position = header_size;
  while (position <= boundary && boundary - position >= record_overhead) {
    std::array<std::byte, 8> length;
    if (!readAt(fd.value, length.data(), length.size(), position))
      break;
    if (get32(length.data() + 4) != crc32c({length.data(), 4}))
      break;
    const auto size = get32(length.data());
    if (static_cast<std::uint64_t>(size) >
        boundary - position - record_overhead)
      break;
    std::uint32_t crc = 0;
    std::uint64_t done = 0;
    bool complete = true;
    while (done < size) {
      const auto chunk = static_cast<std::size_t>(
          std::min<std::uint64_t>(buffer.size(), size - done));
      if (!readAt(fd.value, buffer.data(), chunk, position + 8 + done)) {
        complete = false;
        break;
      }
      crc = crc32c({buffer.data(), chunk}, crc);
      done += chunk;
    }
    std::array<std::byte, 4> checksum;
    if (!complete ||
        !readAt(fd.value, checksum.data(), checksum.size(),
                position + 8 + size) ||
        crc != get32(checksum.data()))
      break;
    offsets.push_back(position);
    position += static_cast<std::uint64_t>(size) + record_overhead;
  }
  if (position > max_file_size - 4 ||
      offsets.size() > (max_file_size - position - 4) / 8)
    throw Error(ErrorCode::Unrecoverable, "rebuilt footer exceeds off_t");
  while (::ftruncate(fd.value, static_cast<off_t>(position)) < 0) {
    if (errno == EINTR)
      continue;
    ioError("ftruncate");
  }
  seekTo(fd.value, position);
  writeFooter(fd.value, offsets);
  syncFd(fd.value);
  const auto sealed_header = makeHeader(position);
  writeAt(fd.value, sealed_header, 0);
  syncFd(fd.value);
  return {offsets.size(), position, original_size};
}
} // namespace raf
