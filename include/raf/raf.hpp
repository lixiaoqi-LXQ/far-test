#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace raf {

using RecordId = std::uint64_t;
using ByteSpan = std::span<const std::byte>;
template <class T> using Span = std::span<T>;

inline ByteSpan asBytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}

enum class ErrorCode {
  Io,
  Busy,
  NeedsRecovery,
  CorruptRecord,
  Unrecoverable,
  Closed,
  InvalidArgument,
  OutOfRange
};

class Error : public std::runtime_error {
public:
  Error(ErrorCode code, std::string message, int system_error = 0);
  ErrorCode code() const noexcept { return code_; }
  int systemError() const noexcept { return system_error_; }

private:
  ErrorCode code_;
  int system_error_;
};

struct RecordRange {
  RecordId first;
  std::uint64_t count;
};

struct RecordView {
  const std::byte *data;
  std::size_t size;
  ByteSpan bytes() const noexcept { return {data, size}; }
  std::string_view stringView() const noexcept {
    return {reinterpret_cast<const char *>(data), size};
  }
};

struct WriterOptions {
  // Zero disables periodic sync; explicit sync/seal still synchronize.
  std::chrono::milliseconds sync_interval{1000};
};

struct SyncStatus {
  std::uint64_t durable_records = 0;
  std::uint64_t successful_syncs = 0;
  std::uint64_t last_duration_ns = 0;
  std::uint64_t total_duration_ns = 0;
  std::uint64_t max_duration_ns = 0;
  int last_error = 0;
};

class RecordWriter {
public:
  explicit RecordWriter(const std::string &filepath,
                        WriterOptions options = {});
  ~RecordWriter(); // Releases resources; explicit seal/close is required.
  RecordWriter(const RecordWriter &) = delete;
  RecordWriter &operator=(const RecordWriter &) = delete;

  RecordId append(ByteSpan payload);
  RecordRange appendBatch(Span<const ByteSpan> records);
  void sync();
  void seal();
  void close() { seal(); }
  std::uint64_t recordCount() const;
  SyncStatus syncStatus() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

struct ReaderOptions {
  bool verify_payload_crc = true;
};

class RecordReader {
public:
  class Iterator {
  public:
    bool hasNext() const noexcept;
    RecordView next();

  private:
    friend class RecordReader;
    explicit Iterator(const RecordReader *reader) : reader_(reader) {}
    const RecordReader *reader_;
    RecordId next_ = 0;
  };

  explicit RecordReader(const std::string &filepath,
                        ReaderOptions options = {});
  ~RecordReader();
  RecordReader(const RecordReader &) = delete;
  RecordReader &operator=(const RecordReader &) = delete;

  std::uint64_t recordCount() const noexcept;
  std::size_t indexBytes() const noexcept;
  RecordView read(RecordId id) const;
  Iterator iterator() const { return Iterator(this); }
  void validate() const; // Always verifies length CRCs and payload CRCs.
private:
  struct State;
  std::unique_ptr<State> state_;
};

struct RecoveryResult {
  std::uint64_t records;
  std::uint64_t valid_data_end;
  std::uint64_t original_file_size;
};

RecoveryResult recover(const std::string &filepath);

} // namespace raf
