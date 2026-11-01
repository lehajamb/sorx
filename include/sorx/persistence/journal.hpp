#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sorx::persistence {

using ByteBuffer = std::vector<std::byte>;
using RecordType = std::uint16_t;

inline constexpr std::size_t route_critical_hash_size = 32U;
inline constexpr std::uint16_t current_journal_format_version = 1U;

class PersistenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class EncodingError : public PersistenceError {
public:
    using PersistenceError::PersistenceError;
};

class JournalIoError : public PersistenceError {
public:
    using PersistenceError::PersistenceError;
};

class JournalLockError : public PersistenceError {
public:
    using PersistenceError::PersistenceError;
};

class JournalCorruption : public PersistenceError {
public:
    using PersistenceError::PersistenceError;
};

class IncompatibleJournal : public PersistenceError {
public:
    using PersistenceError::PersistenceError;
};

// Canonical payload helpers. Integers are encoded in big-endian byte order and
// variable-size fields carry an unsigned 32-bit byte length. They deliberately
// never serialize object representations, padding, pointers, or native enums.
class CanonicalWriter {
public:
    void write_u8(std::uint8_t value);
    void write_u16(std::uint16_t value);
    void write_u32(std::uint32_t value);
    void write_u64(std::uint64_t value);
    void write_bool(bool value);
    void write_bytes(std::span<const std::byte> value);
    void write_string(std::string_view value);

    [[nodiscard]] const ByteBuffer& bytes() const noexcept;
    [[nodiscard]] ByteBuffer take() && noexcept;

private:
    ByteBuffer bytes_;
};

class CanonicalReader {
public:
    explicit CanonicalReader(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::uint8_t read_u8();
    [[nodiscard]] std::uint16_t read_u16();
    [[nodiscard]] std::uint32_t read_u32();
    [[nodiscard]] std::uint64_t read_u64();
    [[nodiscard]] bool read_bool();
    [[nodiscard]] ByteBuffer read_bytes();
    [[nodiscard]] std::string read_string();
    [[nodiscard]] std::size_t remaining() const noexcept;
    void require_end() const;

private:
    [[nodiscard]] std::span<const std::byte> read_raw(std::size_t size);

    std::span<const std::byte> bytes_;
    std::size_t offset_{0U};
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

struct JournalHeader {
    std::uint32_t schema_version{1U};
    std::array<std::byte, route_critical_hash_size> route_critical_hash{};
    std::string account_context;

    [[nodiscard]] bool operator==(const JournalHeader&) const = default;
};

struct RecordInput {
    RecordType type{};
    std::uint16_t version{1U};
    std::uint64_t core_ingress_sequence{};
    ByteBuffer payload;
};

struct JournalRecord {
    RecordType type{};
    std::uint16_t version{};
    std::uint64_t record_id{};
    std::uint64_t batch_id{};
    std::uint64_t core_ingress_sequence{};
    ByteBuffer payload;
};

struct BatchCommit {
    std::uint64_t batch_id{};
    std::uint64_t first_record_id{};
    std::uint64_t last_record_id{};
    std::uint32_t record_count{};
};

struct ReplayResult {
    JournalHeader header;
    std::vector<JournalRecord> records;
    std::uint64_t last_record_id{};
    std::uint64_t last_batch_id{};
    std::uint64_t next_record_id{1U};
    std::uint64_t next_batch_id{1U};
    std::uintmax_t valid_file_bytes{};
    std::uintmax_t ignored_trailing_bytes{};
};

// Journal owns a non-blocking, exclusive process lock for its lifetime. A
// successful append has been fflush'ed and handed to the operating system's
// durable file flush primitive before append_batch returns.
class Journal {
public:
    Journal(std::filesystem::path path, JournalHeader expected_header);
    ~Journal();

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) noexcept;
    Journal& operator=(Journal&&) noexcept;

    [[nodiscard]] BatchCommit append_batch(std::span<const RecordInput> records);
    [[nodiscard]] ReplayResult replay() const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const JournalHeader& header() const noexcept;
    [[nodiscard]] std::uint64_t next_record_id() const noexcept;
    [[nodiscard]] std::uint64_t next_batch_id() const noexcept;
    [[nodiscard]] std::uintmax_t recovered_trailing_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct Checkpoint {
    std::uint64_t last_record_id{};
    std::uint64_t last_batch_id{};
    ByteBuffer payload;
};

class CheckpointStore {
public:
    // Writes path + ".tmp", flushes it durably, and atomically replaces path.
    static void write_atomic(
        const std::filesystem::path& path,
        const JournalHeader& header,
        const Checkpoint& checkpoint);

    // Missing checkpoints are normal. Present checkpoints are fully validated
    // against the expected schema, account context, and route-critical hash.
    [[nodiscard]] static std::optional<Checkpoint> read(
        const std::filesystem::path& path,
        const JournalHeader& expected_header);
};

}  // namespace sorx::persistence
