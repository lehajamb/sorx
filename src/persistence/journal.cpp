#include "sorx/persistence/journal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sorx::persistence {
namespace {

constexpr std::string_view journal_magic{"SORXJNL1"};
constexpr std::string_view batch_magic{"SORXBCH1"};
constexpr std::string_view commit_magic{"SORXCMT1"};
constexpr std::string_view checkpoint_magic{"SORXCKP1"};

constexpr std::size_t maximum_account_context_bytes = 4U * 1024U;
constexpr std::size_t maximum_record_payload_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_batch_bytes = 256U * 1024U * 1024U;
constexpr std::size_t maximum_checkpoint_payload_bytes = 256U * 1024U * 1024U;
constexpr std::uint32_t maximum_records_per_batch = 1'000'000U;

constexpr std::size_t journal_header_fixed_bytes =
    8U + 4U + 2U + 2U + 4U + 2U + 4U + route_critical_hash_size + 4U;
constexpr std::size_t batch_prefix_bytes = 8U + 4U + 2U + 2U + 8U + 4U + 4U;
constexpr std::size_t record_fixed_bytes = 4U + 2U + 2U + 8U + 8U + 8U + 4U + 4U;
constexpr std::size_t batch_commit_bytes = 8U + 8U + 4U + 4U + 4U;
constexpr std::size_t checkpoint_fixed_bytes =
    8U + 4U + 2U + 2U + 4U + 2U + 4U + route_critical_hash_size
    + 8U + 8U + 4U + 4U + 4U;

[[nodiscard]] std::byte as_byte(const std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

[[nodiscard]] std::uint8_t as_u8(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

void append_u8(ByteBuffer& output, const std::uint8_t value) {
    output.push_back(as_byte(value));
}

void append_u16(ByteBuffer& output, const std::uint16_t value) {
    append_u8(output, static_cast<std::uint8_t>(value >> 8U));
    append_u8(output, static_cast<std::uint8_t>(value));
}

void append_u32(ByteBuffer& output, const std::uint32_t value) {
    append_u8(output, static_cast<std::uint8_t>(value >> 24U));
    append_u8(output, static_cast<std::uint8_t>(value >> 16U));
    append_u8(output, static_cast<std::uint8_t>(value >> 8U));
    append_u8(output, static_cast<std::uint8_t>(value));
}

void append_u64(ByteBuffer& output, const std::uint64_t value) {
    append_u8(output, static_cast<std::uint8_t>(value >> 56U));
    append_u8(output, static_cast<std::uint8_t>(value >> 48U));
    append_u8(output, static_cast<std::uint8_t>(value >> 40U));
    append_u8(output, static_cast<std::uint8_t>(value >> 32U));
    append_u8(output, static_cast<std::uint8_t>(value >> 24U));
    append_u8(output, static_cast<std::uint8_t>(value >> 16U));
    append_u8(output, static_cast<std::uint8_t>(value >> 8U));
    append_u8(output, static_cast<std::uint8_t>(value));
}

void append_raw(ByteBuffer& output, const std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

void append_text(ByteBuffer& output, const std::string_view text) {
    output.reserve(output.size() + text.size());
    for (const unsigned char value : text) {
        output.push_back(as_byte(value));
    }
}

void append_magic(ByteBuffer& output, const std::string_view magic) {
    if (magic.size() != 8U) {
        throw EncodingError("persistence magic must contain exactly eight bytes");
    }
    append_text(output, magic);
}

[[nodiscard]] std::uint32_t checked_u32(
    const std::size_t value,
    const std::string_view field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw EncodingError(std::string(field) + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::size_t checked_add(
    const std::size_t left,
    const std::size_t right,
    const std::string_view field) {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        throw EncodingError(std::string(field) + " size overflows");
    }
    return left + right;
}

class Cursor {
public:
    explicit Cursor(const std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::span<const std::byte> read_raw(const std::size_t size) {
        if (size > remaining()) {
            throw JournalCorruption("canonical field extends past its frame");
        }
        const auto result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] std::uint8_t read_u8() {
        return as_u8(read_raw(1U).front());
    }

    [[nodiscard]] std::uint16_t read_u16() {
        const auto bytes = read_raw(2U);
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(as_u8(bytes[0])) << 8U)
            | static_cast<std::uint16_t>(as_u8(bytes[1])));
    }

    [[nodiscard]] std::uint32_t read_u32() {
        const auto bytes = read_raw(4U);
        return (static_cast<std::uint32_t>(as_u8(bytes[0])) << 24U)
            | (static_cast<std::uint32_t>(as_u8(bytes[1])) << 16U)
            | (static_cast<std::uint32_t>(as_u8(bytes[2])) << 8U)
            | static_cast<std::uint32_t>(as_u8(bytes[3]));
    }

    [[nodiscard]] std::uint64_t read_u64() {
        const auto bytes = read_raw(8U);
        std::uint64_t value = 0U;
        for (const auto byte : bytes) {
            value = (value << 8U) | as_u8(byte);
        }
        return value;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] std::size_t offset() const noexcept {
        return offset_;
    }

    void require_end(const std::string_view frame) const {
        if (remaining() != 0U) {
            throw JournalCorruption(std::string(frame) + " has trailing bytes");
        }
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{0U};
};

[[nodiscard]] bool matches_magic(
    const std::span<const std::byte> bytes,
    const std::string_view magic) noexcept {
    if (bytes.size() < magic.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < magic.size(); ++index) {
        if (as_u8(bytes[index]) != static_cast<unsigned char>(magic[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool matches_magic_prefix(
    const std::span<const std::byte> bytes,
    const std::string_view magic) noexcept {
    if (bytes.size() > magic.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        if (as_u8(bytes[index]) != static_cast<unsigned char>(magic[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t find_magic(
    const std::span<const std::byte> bytes,
    const std::size_t start,
    const std::string_view magic) noexcept {
    if (start >= bytes.size() || magic.size() > bytes.size() - start) {
        return std::string_view::npos;
    }
    for (std::size_t offset = start; offset + magic.size() <= bytes.size(); ++offset) {
        if (matches_magic(bytes.subspan(offset), magic)) {
            return offset;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] std::string path_string(const std::filesystem::path& path) {
    return path.string();
}

[[noreturn]] void throw_errno_error(
    const std::string_view operation,
    const std::filesystem::path& path,
    const int error_number = errno) {
    throw JournalIoError(
        std::string(operation) + " " + path_string(path) + ": "
        + std::system_category().message(error_number));
}

#ifdef _WIN32
[[noreturn]] void throw_windows_error(
    const std::string_view operation,
    const std::filesystem::path& path,
    const unsigned long error_number = GetLastError()) {
    throw JournalIoError(
        std::string(operation) + " " + path_string(path) + ": "
        + std::system_category().message(static_cast<int>(error_number)));
}
#endif

[[nodiscard]] std::FILE* open_file(
    const std::filesystem::path& path,
    const char* mode
#ifdef _WIN32
    , const wchar_t* wide_mode
#endif
) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    (void)mode;
    file = _wfsopen(path.c_str(), wide_mode, _SH_DENYNO);
    if (file == nullptr) {
        throw_errno_error("cannot open", path);
    }
#else
    file = std::fopen(path.c_str(), mode);
    if (file == nullptr) {
        throw_errno_error("cannot open", path);
    }
#endif
    return file;
}

void write_all(
    std::FILE* const file,
    const std::span<const std::byte> bytes,
    const std::filesystem::path& path) {
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto written = std::fwrite(
            bytes.data() + offset,
            1U,
            bytes.size() - offset,
            file);
        if (written == 0U) {
            throw_errno_error("cannot write", path);
        }
        offset += written;
    }
}

void flush_durable(std::FILE* const file, const std::filesystem::path& path) {
    if (std::fflush(file) != 0) {
        throw_errno_error("cannot flush stdio buffer for", path);
    }
#ifdef _WIN32
    const int descriptor = _fileno(file);
    if (descriptor < 0 || _commit(descriptor) != 0) {
        throw_errno_error("cannot durably flush", path);
    }
#else
    const int descriptor = fileno(file);
    if (descriptor < 0 || fsync(descriptor) != 0) {
        throw_errno_error("cannot durably flush", path);
    }
#endif
}

void close_checked(std::FILE*& file, const std::filesystem::path& path) {
    std::FILE* const closing = file;
    file = nullptr;
    if (std::fclose(closing) != 0) {
        throw_errno_error("cannot close", path);
    }
}

void validate_parent_directory(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = std::filesystem::current_path();
    }
    std::error_code error;
    const bool is_directory = std::filesystem::is_directory(parent, error);
    if (error || !is_directory) {
        throw JournalIoError(
            "persistence parent directory does not exist: " + path_string(parent));
    }
}

void write_new_durable(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    std::FILE* file = open_file(
        path,
        "wb"
#ifdef _WIN32
        , L"wb"
#endif
    );
    try {
        write_all(file, bytes, path);
        flush_durable(file, path);
        close_checked(file, path);
    } catch (...) {
        (void)std::fclose(file);
        throw;
    }
}

[[nodiscard]] ByteBuffer read_all(const std::filesystem::path& path) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw JournalIoError(
            "cannot determine persistence file size for " + path_string(path)
            + ": " + error.message());
    }
    if (file_size > std::numeric_limits<std::size_t>::max()) {
        throw JournalIoError("persistence file is too large to address");
    }
    const auto size = static_cast<std::size_t>(file_size);
    ByteBuffer bytes(size);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw JournalIoError("cannot read " + path_string(path));
    }
    std::size_t offset = 0U;
    constexpr auto maximum_chunk = static_cast<std::size_t>(1024U * 1024U);
    while (offset < size) {
        const auto chunk = std::min(maximum_chunk, size - offset);
        input.read(
            reinterpret_cast<char*>(bytes.data() + offset),
            static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw JournalIoError("short read from " + path_string(path));
        }
        offset += chunk;
    }
    return bytes;
}

void truncate_durable(
    const std::filesystem::path& path,
    const std::uintmax_t valid_size) {
    if (valid_size > std::numeric_limits<std::uint64_t>::max()) {
        throw JournalIoError("journal truncation offset exceeds uint64");
    }
    std::FILE* file = open_file(
        path,
        "rb+"
#ifdef _WIN32
        , L"rb+"
#endif
    );
    try {
        const int descriptor =
#ifdef _WIN32
            _fileno(file);
        if (descriptor < 0
            || _chsize_s(descriptor, static_cast<std::uint64_t>(valid_size)) != 0) {
            throw_errno_error("cannot truncate", path);
        }
#else
            fileno(file);
        if (descriptor < 0
            || ftruncate(descriptor, static_cast<off_t>(valid_size)) != 0) {
            throw_errno_error("cannot truncate", path);
        }
#endif
        flush_durable(file, path);
        close_checked(file, path);
    } catch (...) {
        (void)std::fclose(file);
        throw;
    }
}

class ExclusiveFileLock {
public:
    explicit ExclusiveFileLock(const std::filesystem::path& path) : path_(path) {
#ifdef _WIN32
        handle_ = CreateFileW(
            path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw JournalLockError(
                "cannot open journal lock " + path_string(path_) + ": "
                + std::system_category().message(static_cast<int>(GetLastError())));
        }
        if (LockFileEx(
                handle_,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0U,
                1U,
                0U,
                &overlapped_)
            == 0) {
            const auto error = GetLastError();
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw JournalLockError(
                "journal is already locked: " + path_string(path_) + ": "
                + std::system_category().message(static_cast<int>(error)));
        }
#else
        descriptor_ = open(path_.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (descriptor_ < 0) {
            throw JournalLockError(
                "cannot open journal lock " + path_string(path_) + ": "
                + std::system_category().message(errno));
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            close(descriptor_);
            descriptor_ = -1;
            throw JournalLockError(
                "journal is already locked: " + path_string(path_) + ": "
                + std::system_category().message(error));
        }
#endif
    }

    ~ExclusiveFileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)UnlockFileEx(handle_, 0U, 1U, 0U, &overlapped_);
            (void)CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            (void)flock(descriptor_, LOCK_UN);
            (void)close(descriptor_);
        }
#endif
    }

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

private:
    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
    OVERLAPPED overlapped_{};
#else
    int descriptor_{-1};
#endif
};

[[nodiscard]] std::filesystem::path lock_path_for(
    const std::filesystem::path& journal_path) {
    auto result = journal_path;
    result += ".lock";
    return result;
}

void validate_header_value(const JournalHeader& header) {
    if (header.schema_version == 0U) {
        throw EncodingError("journal schema version must be positive");
    }
    if (header.account_context.empty()) {
        throw EncodingError("journal account context must not be empty");
    }
    if (header.account_context.size() > maximum_account_context_bytes) {
        throw EncodingError("journal account context is too large");
    }
}

[[nodiscard]] ByteBuffer encode_journal_header(const JournalHeader& header) {
    validate_header_value(header);
    const auto total_size = checked_add(
        journal_header_fixed_bytes,
        header.account_context.size(),
        "journal header");

    ByteBuffer output;
    output.reserve(total_size);
    append_magic(output, journal_magic);
    append_u32(output, checked_u32(total_size, "journal header"));
    append_u16(output, current_journal_format_version);
    append_u16(output, 0U);
    append_u32(output, header.schema_version);
    append_u16(output, static_cast<std::uint16_t>(route_critical_hash_size));
    append_u32(output, checked_u32(header.account_context.size(), "account context"));
    append_raw(output, header.route_critical_hash);
    append_text(output, header.account_context);
    append_u32(output, crc32c(output));
    return output;
}

struct DecodedHeader {
    JournalHeader value;
    std::size_t encoded_size{};
};

[[nodiscard]] DecodedHeader decode_journal_header(
    const std::span<const std::byte> file) {
    if (file.size() < journal_header_fixed_bytes) {
        throw JournalCorruption("journal header is truncated");
    }
    if (!matches_magic(file, journal_magic)) {
        throw JournalCorruption("journal magic is invalid");
    }

    Cursor prefix(file);
    (void)prefix.read_raw(8U);
    const auto total_size = static_cast<std::size_t>(prefix.read_u32());
    if (total_size < journal_header_fixed_bytes || total_size > file.size()) {
        throw JournalCorruption("journal header size is invalid or truncated");
    }
    const auto frame = file.first(total_size);
    Cursor cursor(frame);
    (void)cursor.read_raw(8U);
    (void)cursor.read_u32();
    const auto format_version = cursor.read_u16();
    if (format_version != current_journal_format_version) {
        throw IncompatibleJournal("journal file format version is incompatible");
    }
    if (cursor.read_u16() != 0U) {
        throw JournalCorruption("journal header reserved field is nonzero");
    }

    JournalHeader header;
    header.schema_version = cursor.read_u32();
    const auto hash_size = cursor.read_u16();
    if (hash_size != route_critical_hash_size) {
        throw IncompatibleJournal("journal route-critical hash width is incompatible");
    }
    const auto account_size = static_cast<std::size_t>(cursor.read_u32());
    if (account_size == 0U || account_size > maximum_account_context_bytes) {
        throw JournalCorruption("journal account context length is invalid");
    }
    const auto expected_size = checked_add(
        journal_header_fixed_bytes,
        account_size,
        "journal header");
    if (expected_size != total_size) {
        throw JournalCorruption("journal header fields disagree with its size");
    }
    const auto hash = cursor.read_raw(route_critical_hash_size);
    std::copy(hash.begin(), hash.end(), header.route_critical_hash.begin());
    const auto account = cursor.read_raw(account_size);
    header.account_context.reserve(account_size);
    for (const auto byte : account) {
        header.account_context.push_back(static_cast<char>(as_u8(byte)));
    }
    const auto stored_crc = cursor.read_u32();
    cursor.require_end("journal header");
    if (stored_crc != crc32c(frame.first(frame.size() - 4U))) {
        throw JournalCorruption("journal header CRC32C mismatch");
    }
    if (header.schema_version == 0U) {
        throw JournalCorruption("journal schema version is zero");
    }
    return DecodedHeader{std::move(header), total_size};
}

void require_compatible(
    const JournalHeader& actual,
    const JournalHeader& expected,
    const std::string_view artifact) {
    validate_header_value(expected);
    if (actual.schema_version != expected.schema_version) {
        throw IncompatibleJournal(std::string(artifact) + " schema version differs");
    }
    if (actual.route_critical_hash != expected.route_critical_hash) {
        throw IncompatibleJournal(
            std::string(artifact) + " route-critical hash differs");
    }
    if (actual.account_context != expected.account_context) {
        throw IncompatibleJournal(std::string(artifact) + " account context differs");
    }
}

[[nodiscard]] ByteBuffer encode_batch(
    const std::span<const RecordInput> inputs,
    const std::uint64_t batch_id,
    const std::uint64_t first_record_id) {
    if (inputs.empty()) {
        throw EncodingError("journal batch must contain at least one record");
    }
    if (inputs.size() > maximum_records_per_batch) {
        throw EncodingError("journal batch contains too many records");
    }
    if (batch_id == 0U || first_record_id == 0U) {
        throw EncodingError("journal IDs must be positive");
    }
    if (inputs.size() - 1U
        > std::numeric_limits<std::uint64_t>::max() - first_record_id) {
        throw EncodingError("journal record ID range overflows");
    }

    ByteBuffer record_bytes;
    std::uint64_t record_id = first_record_id;
    for (const auto& input : inputs) {
        if (input.type == 0U) {
            throw EncodingError("journal record type must be positive");
        }
        if (input.version == 0U) {
            throw EncodingError("journal record version must be positive");
        }
        if (input.payload.size() > maximum_record_payload_bytes) {
            throw EncodingError("journal record payload is too large");
        }
        const auto frame_size = checked_add(
            record_fixed_bytes,
            input.payload.size(),
            "journal record");
        append_u32(record_bytes, checked_u32(frame_size, "journal record"));
        append_u16(record_bytes, input.type);
        append_u16(record_bytes, input.version);
        append_u64(record_bytes, record_id);
        append_u64(record_bytes, batch_id);
        append_u64(record_bytes, input.core_ingress_sequence);
        append_u32(
            record_bytes,
            checked_u32(input.payload.size(), "journal record payload"));
        append_u32(record_bytes, crc32c(input.payload));
        append_raw(record_bytes, input.payload);
        ++record_id;
    }

    const auto total_size = checked_add(
        checked_add(batch_prefix_bytes, record_bytes.size(), "journal batch"),
        batch_commit_bytes,
        "journal batch");
    if (total_size > maximum_batch_bytes) {
        throw EncodingError("journal batch is too large");
    }

    ByteBuffer output;
    output.reserve(total_size);
    append_magic(output, batch_magic);
    append_u32(output, checked_u32(total_size, "journal batch"));
    append_u16(output, current_journal_format_version);
    append_u16(output, 0U);
    append_u64(output, batch_id);
    append_u32(output, checked_u32(inputs.size(), "record count"));
    append_u32(output, checked_u32(record_bytes.size(), "record bytes"));
    append_raw(output, record_bytes);
    const auto batch_crc = crc32c(output);
    append_magic(output, commit_magic);
    append_u64(output, batch_id);
    append_u32(output, checked_u32(inputs.size(), "record count"));
    append_u32(output, checked_u32(record_bytes.size(), "record bytes"));
    append_u32(output, batch_crc);
    return output;
}

[[nodiscard]] ReplayResult parse_journal(
    const std::span<const std::byte> file,
    const JournalHeader& expected_header) {
    const auto decoded_header = decode_journal_header(file);
    require_compatible(decoded_header.value, expected_header, "journal");

    ReplayResult result;
    result.header = decoded_header.value;
    std::size_t offset = decoded_header.encoded_size;
    std::uint64_t expected_record_id = 1U;
    std::uint64_t expected_batch_id = 1U;

    while (offset < file.size()) {
        const auto remaining = file.size() - offset;
        const auto tail = file.subspan(offset);
        if (remaining < batch_prefix_bytes) {
            const bool valid_partial_magic = remaining <= batch_magic.size()
                ? matches_magic_prefix(tail, batch_magic)
                : matches_magic(tail, batch_magic);
            if (!valid_partial_magic) {
                throw JournalCorruption(
                    "bytes after the last committed batch are not a torn batch prefix");
            }
            result.ignored_trailing_bytes = remaining;
            break;
        }
        if (!matches_magic(tail, batch_magic)) {
            throw JournalCorruption("journal batch magic is invalid");
        }

        Cursor prefix(tail.first(batch_prefix_bytes));
        (void)prefix.read_raw(8U);
        const auto total_size = static_cast<std::size_t>(prefix.read_u32());
        const auto format_version = prefix.read_u16();
        if (format_version != current_journal_format_version) {
            throw IncompatibleJournal("journal batch format version is incompatible");
        }
        if (prefix.read_u16() != 0U) {
            throw JournalCorruption("journal batch reserved field is nonzero");
        }
        const auto batch_id = prefix.read_u64();
        const auto record_count = prefix.read_u32();
        const auto record_bytes_size = static_cast<std::size_t>(prefix.read_u32());
        prefix.require_end("journal batch prefix");

        if (record_count == 0U || record_count > maximum_records_per_batch) {
            throw JournalCorruption("journal batch record count is invalid");
        }
        const auto computed_total = checked_add(
            checked_add(batch_prefix_bytes, record_bytes_size, "journal batch"),
            batch_commit_bytes,
            "journal batch");
        if (computed_total != total_size || total_size > maximum_batch_bytes) {
            throw JournalCorruption("journal batch size fields disagree");
        }
        if (total_size > remaining) {
            if (find_magic(file, offset + 1U, batch_magic) != std::string_view::npos) {
                throw JournalCorruption(
                    "an incomplete batch is followed by another batch");
            }
            result.ignored_trailing_bytes = remaining;
            break;
        }
        if (batch_id != expected_batch_id) {
            throw JournalCorruption("journal batch IDs are not contiguous and monotonic");
        }

        const auto frame = file.subspan(offset, total_size);
        const auto records_frame = frame.subspan(batch_prefix_bytes, record_bytes_size);
        const auto commit_frame = frame.last(batch_commit_bytes);
        Cursor commit(commit_frame);
        const auto commit_tag = commit.read_raw(8U);
        if (!matches_magic(commit_tag, commit_magic)) {
            throw JournalCorruption("journal batch commit marker is invalid");
        }
        if (commit.read_u64() != batch_id
            || commit.read_u32() != record_count
            || commit.read_u32() != record_bytes_size) {
            throw JournalCorruption("journal batch commit metadata disagrees");
        }
        const auto stored_batch_crc = commit.read_u32();
        commit.require_end("journal batch commit");
        const auto checked_bytes = frame.first(batch_prefix_bytes + record_bytes_size);
        if (stored_batch_crc != crc32c(checked_bytes)) {
            throw JournalCorruption("journal batch CRC32C mismatch");
        }

        Cursor records(records_frame);
        for (std::uint32_t index = 0U; index < record_count; ++index) {
            if (records.remaining() < record_fixed_bytes) {
                throw JournalCorruption("journal record header is truncated");
            }
            const auto frame_size = static_cast<std::size_t>(records.read_u32());
            if (frame_size < record_fixed_bytes
                || frame_size - 4U > records.remaining()) {
                throw JournalCorruption("journal record frame size is invalid");
            }
            const auto record_start = records.offset();
            JournalRecord record;
            record.type = records.read_u16();
            record.version = records.read_u16();
            record.record_id = records.read_u64();
            record.batch_id = records.read_u64();
            record.core_ingress_sequence = records.read_u64();
            const auto payload_size = static_cast<std::size_t>(records.read_u32());
            const auto payload_crc = records.read_u32();
            if (record.type == 0U || record.version == 0U) {
                throw JournalCorruption("journal record type or version is zero");
            }
            if (record.record_id != expected_record_id) {
                throw JournalCorruption(
                    "journal record IDs are not contiguous and monotonic");
            }
            if (record.batch_id != batch_id) {
                throw JournalCorruption("journal record batch ID disagrees");
            }
            if (payload_size > maximum_record_payload_bytes
                || frame_size != record_fixed_bytes + payload_size) {
                throw JournalCorruption("journal record payload size is invalid");
            }
            const auto payload = records.read_raw(payload_size);
            if (records.offset() - record_start != frame_size - 4U) {
                throw JournalCorruption("journal record frame was not consumed exactly");
            }
            if (payload_crc != crc32c(payload)) {
                throw JournalCorruption("journal record payload CRC32C mismatch");
            }
            record.payload.assign(payload.begin(), payload.end());
            result.records.push_back(std::move(record));
            ++expected_record_id;
        }
        records.require_end("journal records");

        result.last_record_id = expected_record_id - 1U;
        result.last_batch_id = batch_id;
        ++expected_batch_id;
        offset += total_size;
    }

    result.valid_file_bytes = offset;
    result.next_record_id =
        result.last_record_id == std::numeric_limits<std::uint64_t>::max()
        ? 0U
        : result.last_record_id + 1U;
    result.next_batch_id =
        result.last_batch_id == std::numeric_limits<std::uint64_t>::max()
        ? 0U
        : result.last_batch_id + 1U;
    return result;
}

[[nodiscard]] ByteBuffer encode_checkpoint(
    const JournalHeader& header,
    const Checkpoint& checkpoint) {
    validate_header_value(header);
    if (checkpoint.payload.size() > maximum_checkpoint_payload_bytes) {
        throw EncodingError("checkpoint payload is too large");
    }
    if ((checkpoint.last_record_id == 0U) != (checkpoint.last_batch_id == 0U)) {
        throw EncodingError(
            "checkpoint record and batch positions must both be zero or positive");
    }

    auto total_size = checked_add(
        checkpoint_fixed_bytes,
        header.account_context.size(),
        "checkpoint");
    total_size = checked_add(total_size, checkpoint.payload.size(), "checkpoint");

    ByteBuffer output;
    output.reserve(total_size);
    append_magic(output, checkpoint_magic);
    append_u32(output, checked_u32(total_size, "checkpoint"));
    append_u16(output, current_journal_format_version);
    append_u16(output, 0U);
    append_u32(output, header.schema_version);
    append_u16(output, static_cast<std::uint16_t>(route_critical_hash_size));
    append_u32(output, checked_u32(header.account_context.size(), "account context"));
    append_raw(output, header.route_critical_hash);
    append_text(output, header.account_context);
    append_u64(output, checkpoint.last_record_id);
    append_u64(output, checkpoint.last_batch_id);
    append_u32(output, checked_u32(checkpoint.payload.size(), "checkpoint payload"));
    append_u32(output, crc32c(checkpoint.payload));
    append_raw(output, checkpoint.payload);
    append_u32(output, crc32c(output));
    return output;
}

[[nodiscard]] Checkpoint decode_checkpoint(
    const std::span<const std::byte> bytes,
    const JournalHeader& expected_header) {
    if (bytes.size() < checkpoint_fixed_bytes) {
        throw JournalCorruption("checkpoint is truncated");
    }
    if (!matches_magic(bytes, checkpoint_magic)) {
        throw JournalCorruption("checkpoint magic is invalid");
    }
    Cursor cursor(bytes);
    (void)cursor.read_raw(8U);
    const auto total_size = static_cast<std::size_t>(cursor.read_u32());
    if (total_size != bytes.size()) {
        throw JournalCorruption("checkpoint size is invalid");
    }
    if (cursor.read_u16() != current_journal_format_version) {
        throw IncompatibleJournal("checkpoint file format version is incompatible");
    }
    if (cursor.read_u16() != 0U) {
        throw JournalCorruption("checkpoint reserved field is nonzero");
    }

    JournalHeader actual_header;
    actual_header.schema_version = cursor.read_u32();
    if (cursor.read_u16() != route_critical_hash_size) {
        throw IncompatibleJournal("checkpoint route-critical hash width is incompatible");
    }
    const auto account_size = static_cast<std::size_t>(cursor.read_u32());
    if (account_size == 0U || account_size > maximum_account_context_bytes) {
        throw JournalCorruption("checkpoint account context length is invalid");
    }
    const auto hash = cursor.read_raw(route_critical_hash_size);
    std::copy(hash.begin(), hash.end(), actual_header.route_critical_hash.begin());
    const auto account = cursor.read_raw(account_size);
    actual_header.account_context.reserve(account_size);
    for (const auto byte : account) {
        actual_header.account_context.push_back(static_cast<char>(as_u8(byte)));
    }
    require_compatible(actual_header, expected_header, "checkpoint");

    Checkpoint checkpoint;
    checkpoint.last_record_id = cursor.read_u64();
    checkpoint.last_batch_id = cursor.read_u64();
    if ((checkpoint.last_record_id == 0U) != (checkpoint.last_batch_id == 0U)) {
        throw JournalCorruption("checkpoint positions are inconsistent");
    }
    const auto payload_size = static_cast<std::size_t>(cursor.read_u32());
    const auto payload_crc = cursor.read_u32();
    if (payload_size > maximum_checkpoint_payload_bytes
        || cursor.remaining() < 4U
        || payload_size > cursor.remaining() - 4U) {
        throw JournalCorruption("checkpoint payload size is invalid");
    }
    const auto payload = cursor.read_raw(payload_size);
    const auto stored_checkpoint_crc = cursor.read_u32();
    cursor.require_end("checkpoint");
    if (payload_crc != crc32c(payload)) {
        throw JournalCorruption("checkpoint payload CRC32C mismatch");
    }
    if (stored_checkpoint_crc != crc32c(bytes.first(bytes.size() - 4U))) {
        throw JournalCorruption("checkpoint CRC32C mismatch");
    }
    checkpoint.payload.assign(payload.begin(), payload.end());
    return checkpoint;
}

void replace_atomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        == 0) {
        throw_windows_error("cannot atomically replace", destination);
    }
#else
    if (rename(temporary.c_str(), destination.c_str()) != 0) {
        throw_errno_error("cannot atomically replace", destination);
    }
    auto parent = destination.parent_path();
    if (parent.empty()) {
        parent = std::filesystem::current_path();
    }
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int directory = open(parent.c_str(), flags);
    if (directory < 0) {
        throw_errno_error("cannot open checkpoint directory", parent);
    }
    if (fsync(directory) != 0) {
        const int error = errno;
        close(directory);
        throw_errno_error("cannot durably flush checkpoint directory", parent, error);
    }
    if (close(directory) != 0) {
        throw_errno_error("cannot close checkpoint directory", parent);
    }
#endif
}

}  // namespace

void CanonicalWriter::write_u8(const std::uint8_t value) {
    append_u8(bytes_, value);
}

void CanonicalWriter::write_u16(const std::uint16_t value) {
    append_u16(bytes_, value);
}

void CanonicalWriter::write_u32(const std::uint32_t value) {
    append_u32(bytes_, value);
}

void CanonicalWriter::write_u64(const std::uint64_t value) {
    append_u64(bytes_, value);
}

void CanonicalWriter::write_bool(const bool value) {
    append_u8(bytes_, value ? 1U : 0U);
}

void CanonicalWriter::write_bytes(const std::span<const std::byte> value) {
    append_u32(bytes_, checked_u32(value.size(), "canonical byte field"));
    append_raw(bytes_, value);
}

void CanonicalWriter::write_string(const std::string_view value) {
    append_u32(bytes_, checked_u32(value.size(), "canonical string field"));
    append_text(bytes_, value);
}

const ByteBuffer& CanonicalWriter::bytes() const noexcept {
    return bytes_;
}

ByteBuffer CanonicalWriter::take() && noexcept {
    return std::move(bytes_);
}

CanonicalReader::CanonicalReader(const std::span<const std::byte> bytes) noexcept
    : bytes_(bytes) {}

std::span<const std::byte> CanonicalReader::read_raw(const std::size_t size) {
    if (size > remaining()) {
        throw EncodingError("canonical payload is truncated");
    }
    const auto result = bytes_.subspan(offset_, size);
    offset_ += size;
    return result;
}

std::uint8_t CanonicalReader::read_u8() {
    return as_u8(read_raw(1U).front());
}

std::uint16_t CanonicalReader::read_u16() {
    const auto value = read_raw(2U);
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(as_u8(value[0])) << 8U)
        | static_cast<std::uint16_t>(as_u8(value[1])));
}

std::uint32_t CanonicalReader::read_u32() {
    const auto value = read_raw(4U);
    return (static_cast<std::uint32_t>(as_u8(value[0])) << 24U)
        | (static_cast<std::uint32_t>(as_u8(value[1])) << 16U)
        | (static_cast<std::uint32_t>(as_u8(value[2])) << 8U)
        | static_cast<std::uint32_t>(as_u8(value[3]));
}

std::uint64_t CanonicalReader::read_u64() {
    const auto value = read_raw(8U);
    std::uint64_t result = 0U;
    for (const auto byte : value) {
        result = (result << 8U) | as_u8(byte);
    }
    return result;
}

bool CanonicalReader::read_bool() {
    const auto value = read_u8();
    if (value > 1U) {
        throw EncodingError("canonical Boolean must be zero or one");
    }
    return value == 1U;
}

ByteBuffer CanonicalReader::read_bytes() {
    const auto size = static_cast<std::size_t>(read_u32());
    const auto value = read_raw(size);
    return ByteBuffer(value.begin(), value.end());
}

std::string CanonicalReader::read_string() {
    const auto size = static_cast<std::size_t>(read_u32());
    const auto value = read_raw(size);
    std::string result;
    result.reserve(size);
    for (const auto byte : value) {
        result.push_back(static_cast<char>(as_u8(byte)));
    }
    return result;
}

std::size_t CanonicalReader::remaining() const noexcept {
    return bytes_.size() - offset_;
}

void CanonicalReader::require_end() const {
    if (remaining() != 0U) {
        throw EncodingError("canonical payload has trailing bytes");
    }
}

std::uint32_t crc32c(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffff'ffffU;
    for (const auto byte : bytes) {
        crc ^= as_u8(byte);
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82f6'3b78U & mask);
        }
    }
    return ~crc;
}

struct Journal::Impl {
    Impl(std::filesystem::path journal_path, JournalHeader expected)
        : path(std::move(journal_path)),
          expected_header(std::move(expected)),
          lock(lock_path_for(path)) {
        validate_parent_directory(path);
        validate_header_value(expected_header);

        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            throw JournalIoError(
                "cannot inspect journal " + path_string(path) + ": " + error.message());
        }
        if (!exists || std::filesystem::file_size(path, error) == 0U) {
            if (error) {
                throw JournalIoError(
                    "cannot inspect journal " + path_string(path) + ": "
                    + error.message());
            }
            const auto header_bytes = encode_journal_header(expected_header);
            write_new_durable(path, header_bytes);
        }

        auto replayed = parse_journal(read_all(path), expected_header);
        recovered_trailing = replayed.ignored_trailing_bytes;
        if (recovered_trailing != 0U) {
            truncate_durable(path, replayed.valid_file_bytes);
        }
        next_record = replayed.next_record_id;
        next_batch = replayed.next_batch_id;
        file = open_file(
            path,
            "ab"
#ifdef _WIN32
            , L"ab"
#endif
        );
    }

    ~Impl() {
        if (file != nullptr) {
            (void)std::fclose(file);
        }
    }

    std::filesystem::path path;
    JournalHeader expected_header;
    ExclusiveFileLock lock;
    std::FILE* file{nullptr};
    mutable std::mutex mutex;
    std::uint64_t next_record{1U};
    std::uint64_t next_batch{1U};
    std::uintmax_t recovered_trailing{};
    bool poisoned{false};
};

Journal::Journal(std::filesystem::path path, JournalHeader expected_header)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(expected_header))) {}

Journal::~Journal() = default;
Journal::Journal(Journal&&) noexcept = default;
Journal& Journal::operator=(Journal&&) noexcept = default;

BatchCommit Journal::append_batch(const std::span<const RecordInput> records) {
    std::scoped_lock guard(impl_->mutex);
    if (impl_->poisoned) {
        throw JournalIoError("journal cannot append after an earlier write failure");
    }
    if (impl_->next_record == 0U || impl_->next_batch == 0U) {
        throw PersistenceError("journal monotonic ID space is exhausted");
    }
    if (!records.empty()
        && records.size() - 1U
            > std::numeric_limits<std::uint64_t>::max() - impl_->next_record) {
        throw PersistenceError("journal monotonic record ID space is exhausted");
    }

    const auto first_record = impl_->next_record;
    const auto batch_id = impl_->next_batch;
    const auto bytes = encode_batch(records, batch_id, first_record);
    try {
        write_all(impl_->file, bytes, impl_->path);
        flush_durable(impl_->file, impl_->path);
    } catch (...) {
        impl_->poisoned = true;
        throw;
    }

    const auto count = static_cast<std::uint64_t>(records.size());
    const auto last_record = first_record + count - 1U;
    impl_->next_record = last_record == std::numeric_limits<std::uint64_t>::max()
        ? 0U
        : last_record + 1U;
    impl_->next_batch = batch_id == std::numeric_limits<std::uint64_t>::max()
        ? 0U
        : batch_id + 1U;
    return BatchCommit{
        batch_id,
        first_record,
        last_record,
        static_cast<std::uint32_t>(records.size())};
}

ReplayResult Journal::replay() const {
    std::scoped_lock guard(impl_->mutex);
    return parse_journal(read_all(impl_->path), impl_->expected_header);
}

const std::filesystem::path& Journal::path() const noexcept {
    return impl_->path;
}

const JournalHeader& Journal::header() const noexcept {
    return impl_->expected_header;
}

std::uint64_t Journal::next_record_id() const noexcept {
    return impl_->next_record;
}

std::uint64_t Journal::next_batch_id() const noexcept {
    return impl_->next_batch;
}

std::uintmax_t Journal::recovered_trailing_bytes() const noexcept {
    return impl_->recovered_trailing;
}

void CheckpointStore::write_atomic(
    const std::filesystem::path& path,
    const JournalHeader& header,
    const Checkpoint& checkpoint) {
    validate_parent_directory(path);
    const auto bytes = encode_checkpoint(header, checkpoint);
    auto temporary = path;
    temporary += ".tmp";
    try {
        write_new_durable(temporary, bytes);
        replace_atomically(temporary, path);
    } catch (...) {
        std::error_code ignored;
        (void)std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::optional<Checkpoint> CheckpointStore::read(
    const std::filesystem::path& path,
    const JournalHeader& expected_header) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw JournalIoError(
            "cannot inspect checkpoint " + path_string(path) + ": " + error.message());
    }
    if (!exists) {
        return std::nullopt;
    }
    return decode_checkpoint(read_all(path), expected_header);
}

}  // namespace sorx::persistence



