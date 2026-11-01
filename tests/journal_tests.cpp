#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/persistence/journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using sorx::persistence::BatchCommit;
using sorx::persistence::ByteBuffer;
using sorx::persistence::CanonicalReader;
using sorx::persistence::CanonicalWriter;
using sorx::persistence::Checkpoint;
using sorx::persistence::CheckpointStore;
using sorx::persistence::IncompatibleJournal;
using sorx::persistence::Journal;
using sorx::persistence::JournalCorruption;
using sorx::persistence::JournalHeader;
using sorx::persistence::JournalLockError;
using sorx::persistence::RecordInput;

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto base = std::filesystem::temp_directory_path();

        for (std::uint64_t attempt = 0; attempt != 100; ++attempt) {
            path_ = base / ("sorx-journal-tests-" + std::to_string(stamp) + "-"
                            + std::to_string(sequence.fetch_add(1)) + "-"
                            + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("unable to create journal test directory: "
                                         + error.message());
            }
        }

        throw std::runtime_error("unable to allocate a unique journal test directory");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] ByteBuffer bytes(std::string_view text) {
    ByteBuffer result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] std::string text(const ByteBuffer& value) {
    std::string result;
    result.reserve(value.size());
    for (const std::byte byte : value) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

[[nodiscard]] JournalHeader make_header(std::string account = "account-A") {
    JournalHeader header;
    header.schema_version = 17;
    header.account_context = std::move(account);
    for (std::size_t index = 0; index != header.route_critical_hash.size(); ++index) {
        header.route_critical_hash[index] = static_cast<std::byte>((index * 13U + 7U) & 0xffU);
    }
    return header;
}

[[nodiscard]] RecordInput make_record(
    std::uint16_t type,
    std::uint16_t version,
    std::uint64_t ingress_sequence,
    std::string_view payload) {
    return RecordInput{type, version, ingress_sequence, bytes(payload)};
}

template <typename Exception, typename Callable>
void expect_throws(Callable&& callable) {
    bool caught = false;
    try {
        std::invoke(std::forward<Callable>(callable));
    } catch (const Exception&) {
        caught = true;
    }
    assert(caught);
}

void append_bytes(const std::filesystem::path& path, std::string_view data) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    assert(output.is_open());
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.flush();
    assert(output.good());
}

void flip_unique_sequence(const std::filesystem::path& path, std::string_view marker) {
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    const std::vector<char> contents(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>{});
    assert(!input.bad());

    const auto first = std::search(contents.begin(), contents.end(), marker.begin(), marker.end());
    assert(first != contents.end());
    const auto second = std::search(first + 1, contents.end(), marker.begin(), marker.end());
    assert(second == contents.end());
    const auto offset = static_cast<std::streamoff>(std::distance(contents.begin(), first));
    const char replacement = static_cast<char>(*first ^ 0x01);

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    assert(file.is_open());
    file.seekp(offset);
    file.write(&replacement, 1);
    file.flush();
    assert(file.good());
}

void test_canonical_encoding_and_crc32c() {
    CanonicalWriter writer;
    writer.write_u8(0x5aU);
    writer.write_u16(0x1234U);
    writer.write_u32(0x89abcdefU);
    writer.write_u64(0x0123456789abcdefULL);
    writer.write_bool(true);
    writer.write_string("sorx");

    const ByteBuffer encoded = std::move(writer).take();
    assert(encoded.size() == 1U + 2U + 4U + 8U + 1U + 4U + 4U);
    assert(std::to_integer<unsigned int>(encoded[0]) == 0x5aU);
    assert(std::to_integer<unsigned int>(encoded[1]) == 0x12U);
    assert(std::to_integer<unsigned int>(encoded[2]) == 0x34U);

    CanonicalReader reader(encoded);
    assert(reader.read_u8() == 0x5aU);
    assert(reader.read_u16() == 0x1234U);
    assert(reader.read_u32() == 0x89abcdefU);
    assert(reader.read_u64() == 0x0123456789abcdefULL);
    assert(reader.read_bool());
    assert(reader.read_string() == "sorx");
    reader.require_end();

    const ByteBuffer check = bytes("123456789");
    assert(sorx::persistence::crc32c(check) == 0xe3069283U);
}

void test_commit_and_replay(const std::filesystem::path& path) {
    const JournalHeader header = make_header();
    {
        Journal journal(path, header);
        const std::array<RecordInput, 2> records{
            make_record(11, 1, 400, "alpha"),
            make_record(12, 3, 401, "beta")};
        const BatchCommit commit = journal.append_batch(records);
        assert(commit.batch_id == 1);
        assert(commit.first_record_id == 1);
        assert(commit.last_record_id == 2);
        assert(commit.record_count == 2);
        assert(journal.next_record_id() == 3);
        assert(journal.next_batch_id() == 2);

        const auto replay = journal.replay();
        assert(replay.records.size() == 2);
        assert(replay.records[0].type == 11);
        assert(replay.records[0].version == 1);
        assert(replay.records[0].record_id == 1);
        assert(replay.records[0].batch_id == 1);
        assert(replay.records[0].core_ingress_sequence == 400);
        assert(text(replay.records[0].payload) == "alpha");
        assert(replay.records[1].record_id == 2);
        assert(replay.records[1].core_ingress_sequence == 401);
        assert(text(replay.records[1].payload) == "beta");
        assert(replay.next_record_id == 3);
        assert(replay.next_batch_id == 2);
        assert(replay.ignored_trailing_bytes == 0);
    }

    JournalHeader incompatible = header;
    ++incompatible.schema_version;
    expect_throws<IncompatibleJournal>([&] { Journal wrong(path, incompatible); });
}

void test_torn_tail_is_ignored_and_removed(const std::filesystem::path& path) {
    const JournalHeader header = make_header("torn-tail-account");
    {
        Journal journal(path, header);
        const std::array<RecordInput, 1> records{make_record(1, 1, 90, "durable")};
        (void)journal.append_batch(records);
    }

    append_bytes(path, std::string_view{"SORXBCH1\x00\x00\x00", 11});
    const auto torn_size = std::filesystem::file_size(path);

    {
        Journal recovered(path, header);
        assert(recovered.recovered_trailing_bytes() == 11);
        assert(std::filesystem::file_size(path) + 11 == torn_size);
        const auto replay = recovered.replay();
        assert(replay.records.size() == 1);
        assert(replay.ignored_trailing_bytes == 0);
        assert(text(replay.records.front().payload) == "durable");

        const std::array<RecordInput, 1> records{make_record(2, 1, 91, "after-recovery")};
        const auto commit = recovered.append_batch(records);
        assert(commit.batch_id == 2);
        assert(commit.first_record_id == 2);
    }

    Journal reopened(path, header);
    const auto replay = reopened.replay();
    assert(replay.records.size() == 2);
    assert(replay.records[1].record_id == 2);
    assert(text(replay.records[1].payload) == "after-recovery");
}

void test_middle_corruption_is_rejected(const std::filesystem::path& path) {
    const JournalHeader header = make_header("corruption-account");
    constexpr std::string_view first_marker = "FIRST-BATCH-PAYLOAD-UNIQUE";
    {
        Journal journal(path, header);
        const std::array<RecordInput, 1> first{make_record(8, 1, 1000, first_marker)};
        const std::array<RecordInput, 1> second{
            make_record(9, 1, 1001, "SECOND-BATCH-PAYLOAD-UNIQUE")};
        (void)journal.append_batch(first);
        (void)journal.append_batch(second);
    }

    flip_unique_sequence(path, first_marker);
    expect_throws<JournalCorruption>([&] { Journal corrupted(path, header); });
}

void test_exclusive_process_lock(const std::filesystem::path& path) {
    const JournalHeader header = make_header("lock-account");
    Journal owner(path, header);
    expect_throws<JournalLockError>([&] { Journal contender(path, header); });
}

void test_checkpoint_round_trip_and_validation(const std::filesystem::path& path) {
    const JournalHeader header = make_header("checkpoint-account");
    assert(!CheckpointStore::read(path, header).has_value());

    const Checkpoint first{73, 21, bytes("CHECKPOINT-PAYLOAD-UNIQUE")};
    CheckpointStore::write_atomic(path, header, first);
    assert(!std::filesystem::exists(path.string() + ".tmp"));
    auto loaded = CheckpointStore::read(path, header);
    assert(loaded.has_value());
    assert(loaded->last_record_id == 73);
    assert(loaded->last_batch_id == 21);
    assert(text(loaded->payload) == "CHECKPOINT-PAYLOAD-UNIQUE");

    const Checkpoint replacement{88, 25, bytes("replacement")};
    CheckpointStore::write_atomic(path, header, replacement);
    loaded = CheckpointStore::read(path, header);
    assert(loaded.has_value());
    assert(loaded->last_record_id == 88);
    assert(loaded->last_batch_id == 25);
    assert(text(loaded->payload) == "replacement");

    JournalHeader incompatible = header;
    incompatible.route_critical_hash[3] ^= std::byte{0x80};
    expect_throws<IncompatibleJournal>([&] { (void)CheckpointStore::read(path, incompatible); });

    flip_unique_sequence(path, "replacement");
    expect_throws<JournalCorruption>([&] { (void)CheckpointStore::read(path, header); });
}

void test_monotonic_id_recovery(const std::filesystem::path& path) {
    const JournalHeader header = make_header("id-account");
    {
        Journal journal(path, header);
        const std::array<RecordInput, 2> first{
            make_record(1, 1, 1, "one"), make_record(1, 1, 2, "two")};
        const std::array<RecordInput, 1> second{make_record(1, 1, 3, "three")};
        (void)journal.append_batch(first);
        (void)journal.append_batch(second);
        assert(journal.next_record_id() == 4);
        assert(journal.next_batch_id() == 3);
    }

    {
        Journal recovered(path, header);
        assert(recovered.next_record_id() == 4);
        assert(recovered.next_batch_id() == 3);
        const std::array<RecordInput, 2> third{
            make_record(2, 7, 4, "four"), make_record(2, 7, 5, "five")};
        const auto commit = recovered.append_batch(third);
        assert(commit.batch_id == 3);
        assert(commit.first_record_id == 4);
        assert(commit.last_record_id == 5);
    }

    Journal final_recovery(path, header);
    const auto replay = final_recovery.replay();
    assert(replay.records.size() == 5);
    assert(replay.last_record_id == 5);
    assert(replay.last_batch_id == 3);
    assert(replay.next_record_id == 6);
    assert(replay.next_batch_id == 4);
    for (std::size_t index = 0; index != replay.records.size(); ++index) {
        assert(replay.records[index].record_id == index + 1U);
    }
}

}  // namespace

int main() {
    test_canonical_encoding_and_crc32c();

    TempDirectory temporary;
    test_commit_and_replay(temporary.path() / "commit.journal");
    test_torn_tail_is_ignored_and_removed(temporary.path() / "torn.journal");
    test_middle_corruption_is_rejected(temporary.path() / "corrupt.journal");
    test_exclusive_process_lock(temporary.path() / "locked.journal");
    test_checkpoint_round_trip_and_validation(temporary.path() / "state.checkpoint");
    test_monotonic_id_recovery(temporary.path() / "ids.journal");
    return 0;
}

