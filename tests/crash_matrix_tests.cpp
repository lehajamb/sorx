#if defined(_MSC_VER)
#ifdef NDEBUG
#undef NDEBUG
#endif
#endif

#include "sorx/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;

constexpr VenueId venue{1U};
constexpr InstrumentId instrument{7U};

class RuntimeTestWireIdentityFactory final
    : public WireIdentityFactory {
public:
    RuntimeTestWireIdentityFactory() {
        profiles_.push_back(WireProfileIdentity{
            venue,
            ProfileId{2U},
            "MockXFIX44",
            "OE-SESSION-1",
            "FIX44.xml",
            "SORX",
            "MOCKX",
        });
    }

    std::span<const WireProfileIdentity> profiles()
        const noexcept override {
        return profiles_;
    }

    WireIdentityResult make(
        const WireIdentityRequest& request) const override {
        WireIdentityResult result;
        result.profile = request.profile;
        if (request.profile != profiles_.front() ||
            !request.child || !request.parent ||
            request.parent_revision == 0U ||
            request.session_epoch == 0U) {
            return result;
        }
        result.wire_order_id =
            (request.kind == DispatchAttemptKind::New ?
                 "TEST-N" :
                 "TEST-C") +
            std::to_string(request.session_epoch) + "-" +
            std::to_string(request.parent_revision) + "-" +
            std::to_string(request.child.value());
        result.ok = true;
        return result;
    }

private:
    std::vector<WireProfileIdentity> profiles_;
};

std::shared_ptr<const WireIdentityFactory>
runtime_test_wire_factory() {
    return std::make_shared<RuntimeTestWireIdentityFactory>();
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto base = std::filesystem::temp_directory_path();
        for (std::uint64_t attempt = 0U; attempt < 100U; ++attempt) {
            path_ = base /
                    ("sorx-crash-matrix-" + std::to_string(stamp) + "-" +
                     std::to_string(sequence.fetch_add(1U)) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error("could not create crash-matrix directory");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path journal(
        const std::string_view name) const {
        return path_ / (std::string{name} + ".journal");
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] RuntimeConfiguration configuration(
    const std::filesystem::path& journal_path) {
    config::ValidatedConfiguration validated;
    validated.schema_version = 1U;
    validated.account_id = "crash-matrix";
    validated.ingress_capacity = 64U;
    validated.core.maximum_aggressive_venues = 1U;
    validated.core.risk.generation = 1U;

    const InstrumentRiskLimit instrument_limit{
        instrument,
        true,
        Quantity{1'000U},
        Quantity{1'000U},
        Notional{1'000'000U},
        Notional{1'000'000U},
        Price{1U},
        Price{10'000U},
        100U,
        100U,
        Quantity{10'000U},
    };
    const VenueRiskLimit venue_limit{
        venue, true, Quantity{10'000U}};
    validated.core.risk.instruments.push_back(instrument_limit);
    validated.core.risk.venues.push_back(venue_limit);
    validated.instruments.push_back(config::InstrumentConfiguration{
        instrument, "ALPHA", instrument_limit});
    validated.venues.push_back(config::VenueConfiguration{
        venue,
        "VENUE_1",
        AdminState::Up,
        venue_limit,
        32U,
        32U,
        8U,
        8U,
        ChannelId{1U},
        ProfileId{1U},
        ProfileId{2U},
        "MockXITCH",
        "MD-SESSION-1",
        "MockXFIX44",
        "OE-SESSION-1",
        "FIX44.xml",
        "SORX",
        "MOCKX",
        std::nullopt,
    });
    validated.routes.push_back(RouteConfig{
        venue,
        instrument,
        "VENUE_1",
        "ALPHA",
        "ALPHA_1",
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{1'000U},
        1U,
        1U,
        10U,
        AdminState::Up,
    });
    validated.route_critical_hash =
        config::compute_route_critical_hash(validated);
    validated.route_critical_hash_hex =
        config::sha256_hex(validated.route_critical_hash);

    RuntimeConfiguration result;
    result.validated = std::move(validated);
    result.journal_path = journal_path;
    result.checkpoint_interval_batches = 256U;
    return result;
}

[[nodiscard]] CandidateRuntimeView live_view() {
    static std::atomic<std::uint64_t> generation{10'000U};
    const std::uint64_t current = generation.fetch_add(1U);
    md::BookView book;
    book.venue = venue;
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = instrument;
    book.feed_session_epoch = current;
    book.recovery_generation = current;
    book.view_generation = current;
    book.last_applied_sequence = current * 10U;
    book.next_sequence = book.last_applied_sequence + 1U;
    book.local_receive_time_ns = current * 100U;
    book.local_publication_time_ns = book.local_receive_time_ns + 1U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.push_bid(md::Level{Price{99U}, Quantity{1'000U}, 1U}));
    assert(book.push_ask(md::Level{Price{100U}, Quantity{1'000U}, 1U}));
    assert(book.valid());

    return CandidateRuntimeView{
        venue,
        current * 10U,
        std::move(book),
        MarketDataState::Live,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        true,
        true,
        true,
    };
}

void publish_live(SorRuntime& runtime) {
    assert(runtime.publish(OrderEntrySessionEvent{
        venue, 1U, OrderEntryState::Ready}));
    assert(runtime.publish(RuntimeViewPublishedEvent{
        instrument, live_view()}));
}

void enable_empty_runtime(SorRuntime& runtime) {
    assert(runtime.recovery_pending());
    publish_live(runtime);
    assert(runtime.complete_recovery());
    assert(!runtime.recovery_pending());
}

[[nodiscard]] ParentOrder order(
    const std::uint64_t id,
    const RoutingIntent intent_value = RoutingIntent::AggressiveIOC,
    const std::uint64_t quantity = 5U,
    const std::int64_t limit = 100) {
    return ParentOrder{
        ParentOrderId{id},
        instrument,
        Side::Buy,
        Quantity{quantity},
        Price{limit},
        intent_value,
    };
}

[[nodiscard]] NewOrderIntent poll_new(SorRuntime& runtime) {
    const auto effect = runtime.poll_egress(venue);
    assert(effect.has_value());
    assert(std::holds_alternative<DispatchNewEffect>(*effect));
    const NewOrderIntent intent_value =
        std::get<DispatchNewEffect>(*effect).intent;
    assert(intent_value.dispatch_attempt_id != 0U);
    return intent_value;
}

[[nodiscard]] CancelOrderIntent poll_cancel(SorRuntime& runtime) {
    const auto effect = runtime.poll_egress(venue);
    assert(effect.has_value());
    assert(std::holds_alternative<DispatchCancelEffect>(*effect));
    const CancelOrderIntent intent_value =
        std::get<DispatchCancelEffect>(*effect).intent;
    assert(intent_value.dispatch_attempt_id != 0U);
    return intent_value;
}

void dispatch_result(
    SorRuntime& runtime,
    const NewOrderIntent& intent_value,
    const LocalDispatchState state = LocalDispatchState::AcceptedLocally) {
    LocalDispatchResult result;
    result.child = intent_value.child;
    result.state = state;
    result.kind = DispatchAttemptKind::New;
    result.attempt_id = intent_value.dispatch_attempt_id;
    result.venue = intent_value.venue;
    result.session_epoch = intent_value.session_epoch;
    result.wire_order_id = intent_value.wire_order_id;
    assert(runtime.publish(LocalDispatchEvent{std::move(result)}));
}

void dispatch_result(
    SorRuntime& runtime,
    const CancelOrderIntent& intent_value,
    const LocalDispatchState state = LocalDispatchState::AcceptedLocally) {
    LocalDispatchResult result;
    result.child = intent_value.child;
    result.state = state;
    result.kind = DispatchAttemptKind::Cancel;
    result.attempt_id = intent_value.dispatch_attempt_id;
    result.venue = intent_value.venue;
    result.session_epoch = intent_value.session_epoch;
    result.wire_order_id = intent_value.cancel_wire_order_id;
    assert(runtime.publish(LocalDispatchEvent{std::move(result)}));
}

[[nodiscard]] NormalizedExecutionReport report(
    const ChildSnapshot& child,
    std::string execution_id,
    const ExecutionKind kind,
    const NormalizedOrderStatus status,
    const Quantity cumulative,
    const std::optional<Quantity> last_quantity = std::nullopt,
    const std::optional<Quantity> leaves = std::nullopt) {
    NormalizedExecutionReport value;
    value.venue = child.venue;
    value.session_epoch = child.session_epoch;
    value.wire_order_id = child.wire_order_id;
    value.execution_id = std::move(execution_id);
    value.durable_report_key = value.execution_id;
    value.kind = kind;
    value.status = status;
    value.last_quantity = last_quantity;
    if (last_quantity.has_value()) {
        value.last_price = child.limit_price;
    }
    value.cumulative_quantity = cumulative;
    value.leaves_quantity = leaves;
    value.value_domain = ExecutionValueDomain::Canonical;
    return value;
}

[[nodiscard]] const ChildSnapshot& child_for(
    const SorSnapshot& snapshot,
    const ParentOrderId parent,
    const std::size_t ordinal = 0U) {
    std::size_t found_count = 0U;
    for (const auto& child : snapshot.children) {
        if (child.parent == parent) {
            if (found_count == ordinal) {
                return child;
            }
            ++found_count;
        }
    }
    assert(false);
    return snapshot.children.front();
}

[[nodiscard]] const ParentSnapshot& parent_for(
    const SorSnapshot& snapshot,
    const ParentOrderId parent) {
    const auto found = std::find_if(
        snapshot.parents.begin(),
        snapshot.parents.end(),
        [parent](const ParentSnapshot& value) {
            return value.order.id == parent;
        });
    assert(found != snapshot.parents.end());
    return *found;
}

[[nodiscard]] persistence::JournalHeader journal_header(
    const RuntimeConfiguration& value) {
    persistence::JournalHeader header;
    header.schema_version = value.validated.schema_version;
    header.account_context = value.validated.account_id;
    for (std::size_t index = 0U;
         index < header.route_critical_hash.size();
         ++index) {
        header.route_critical_hash[index] = static_cast<std::byte>(
            value.validated.route_critical_hash[index]);
    }
    return header;
}

void append_torn_batch_prefix(const std::filesystem::path& path) {
    constexpr std::string_view torn{"SORXBCH1\x00\x00\x00", 11U};
    std::ofstream output(path, std::ios::binary | std::ios::app);
    assert(output.is_open());
    output.write(torn.data(), static_cast<std::streamsize>(torn.size()));
    output.flush();
    assert(output.good());
}

void crash_before_commit_torn_tail(const std::filesystem::path& path) {
    {
        SorRuntime seeded{configuration(path), runtime_test_wire_factory()};
        enable_empty_runtime(seeded);
        assert(seeded.inspect().parents.empty());
    }

    const auto durable_size = std::filesystem::file_size(path);
    append_torn_batch_prefix(path);
    assert(std::filesystem::file_size(path) == durable_size + 11U);

    {
        const RuntimeConfiguration expected = configuration(path);
        persistence::Journal recovered{
            path, journal_header(expected)};
        assert(recovered.recovered_trailing_bytes() == 11U);
        assert(std::filesystem::file_size(path) == durable_size);
    }

    {
        SorRuntime recovered{configuration(path), runtime_test_wire_factory()};
        assert(recovered.inspect().parents.empty());
        publish_live(recovered);
        assert(recovered.complete_recovery());
        assert(recovered.submit(order(1U)).accepted());
        const SorSnapshot state = recovered.inspect();
        assert(state.parents.size() == 1U);
        assert(state.children.size() == 1U);
        assert(state.children.front().id == ChildOrderId{1U});
    }
}

void crash_post_commit_pre_fix(const std::filesystem::path& path) {
    ChildSnapshot prepared;
    {
        SorRuntime seeded{configuration(path), runtime_test_wire_factory()};
        enable_empty_runtime(seeded);
        assert(seeded.submit(order(10U)).accepted());
        prepared = child_for(seeded.inspect(), ParentOrderId{10U});
        assert(prepared.state == ChildState::Prepared);
        assert(!prepared.wire_order_id.empty());
    }

    {
        SorRuntime recovered{configuration(path), runtime_test_wire_factory()};
        const auto before = child_for(
            recovered.inspect(), ParentOrderId{10U});
        assert(before.state == ChildState::Prepared);
        assert(before.id == prepared.id);
        assert(before.wire_order_id == prepared.wire_order_id);
        publish_live(recovered);
        assert(recovered.publish(ReconciliationEvent{
            prepared.id,
            ReconciliationDisposition::DefinitelyUnsent,
            Quantity{},
        }));
        const NewOrderIntent retry = poll_new(recovered);
        assert(retry.child == prepared.id);
        assert(retry.wire_order_id == prepared.wire_order_id);
        dispatch_result(recovered, retry);

        const ChildSnapshot working = child_for(
            recovered.inspect(), ParentOrderId{10U});
        assert(recovered.publish(ExecutionReportEvent{report(
            working,
            "PRE-FIX-FILL",
            ExecutionKind::Trade,
            NormalizedOrderStatus::Filled,
            working.ordered,
            working.ordered,
            Quantity{})}));
        assert(recovered.complete_recovery());
        const SorSnapshot terminal = recovered.inspect();
        assert(child_for(terminal, ParentOrderId{10U}).state ==
               ChildState::Filled);
        assert(child_for(terminal, ParentOrderId{10U}).risk_reservation ==
               Quantity{});
    }
}

void crash_post_fix_pre_ack(const std::filesystem::path& path) {
    ChildSnapshot dispatched;
    {
        SorRuntime seeded{configuration(path), runtime_test_wire_factory()};
        enable_empty_runtime(seeded);
        assert(seeded.submit(order(20U)).accepted());
        const NewOrderIntent wire = poll_new(seeded);
        dispatched = child_for(seeded.inspect(), ParentOrderId{20U});
        assert(dispatched.state == ChildState::DispatchStarted);
        assert(dispatched.active_dispatch_attempt.has_value());
        assert(dispatched.active_dispatch_attempt->attempt_id ==
               wire.dispatch_attempt_id);
    }

    {
        SorRuntime recovered{configuration(path), runtime_test_wire_factory()};
        SorSnapshot state = recovered.inspect();
        const ChildSnapshot& ambiguous =
            child_for(state, ParentOrderId{20U});
        assert(ambiguous.state == ChildState::Ambiguous);
        assert(ambiguous.risk_reservation == ambiguous.ordered);
        assert(parent_for(state, ParentOrderId{20U}).state ==
               ParentState::Suspended);
        publish_live(recovered);
        assert(recovered.publish(ReconciliationEvent{
            ambiguous.id,
            ReconciliationDisposition::Unresolved,
            Quantity{},
        }));
        assert(!recovered.complete_recovery());
        assert(!recovered.poll_egress(venue).has_value());
    }

    // A second replay must retain ownership and must not create another child.
    {
        SorRuntime replayed{configuration(path), runtime_test_wire_factory()};
        const SorSnapshot state = replayed.inspect();
        assert(state.children.size() == 1U);
        const ChildSnapshot& ambiguous = state.children.front();
        assert(ambiguous.id == dispatched.id);
        assert(ambiguous.wire_order_id == dispatched.wire_order_id);
        assert(ambiguous.state == ChildState::Ambiguous);
        assert(ambiguous.risk_reservation == ambiguous.ordered);
    }
}

void crash_after_partial_fill(const std::filesystem::path& path) {
    ChildSnapshot partial;
    {
        SorRuntime seeded{configuration(path), runtime_test_wire_factory()};
        enable_empty_runtime(seeded);
        assert(seeded.submit(order(30U)).accepted());
        const NewOrderIntent wire = poll_new(seeded);
        dispatch_result(seeded, wire);
        const ChildSnapshot child = child_for(
            seeded.inspect(), ParentOrderId{30U});
        assert(seeded.publish(ExecutionReportEvent{report(
            child,
            "PARTIAL-3",
            ExecutionKind::Trade,
            NormalizedOrderStatus::PartiallyFilled,
            Quantity{3U},
            Quantity{3U},
            Quantity{2U})}));
        partial = child_for(seeded.inspect(), ParentOrderId{30U});
        assert(partial.filled == Quantity{3U});
        // The architecture releases risk only at terminal accounting;
        // a partial fill retains the durable full-child reservation.
        assert(partial.risk_reservation == partial.ordered);
    }

    {
        SorRuntime recovered{configuration(path), runtime_test_wire_factory()};
        const ChildSnapshot before = child_for(
            recovered.inspect(), ParentOrderId{30U});
        assert(before.filled == Quantity{3U});
        assert(before.risk_reservation == before.ordered);
        publish_live(recovered);
        assert(recovered.publish(ReconciliationEvent{
            before.id,
            ReconciliationDisposition::Working,
            Quantity{3U},
        }));
        assert(recovered.complete_recovery());
        assert(recovered.publish(ExecutionReportEvent{report(
            before,
            "FINAL-2",
            ExecutionKind::Trade,
            NormalizedOrderStatus::Filled,
            Quantity{5U},
            Quantity{2U},
            Quantity{})}));
        const SorSnapshot terminal = recovered.inspect();
        const ChildSnapshot filled = child_for(
            terminal, ParentOrderId{30U});
        assert(filled.state == ChildState::Filled);
        assert(filled.filled == filled.ordered);
        assert(filled.risk_reservation == Quantity{});
    }

    {
        SorRuntime replayed{configuration(path), runtime_test_wire_factory()};
        const ChildSnapshot filled = child_for(
            replayed.inspect(), ParentOrderId{30U});
        assert(filled.id == partial.id);
        assert(filled.state == ChildState::Filled);
        assert(filled.filled == Quantity{5U});
    }
}

void crash_during_cancel(const std::filesystem::path& path) {
    CancelOrderIntent first_cancel;
    {
        SorRuntime seeded{configuration(path), runtime_test_wire_factory()};
        enable_empty_runtime(seeded);
        assert(seeded.submit(order(
            40U, RoutingIntent::PassiveGTC, 5U, 99)).accepted());
        const NewOrderIntent wire = poll_new(seeded);
        dispatch_result(seeded, wire);
        const ChildSnapshot child = child_for(
            seeded.inspect(), ParentOrderId{40U});
        assert(seeded.publish(ExecutionReportEvent{report(
            child,
            "RESTING-ACK",
            ExecutionKind::Acknowledged,
            NormalizedOrderStatus::Working,
            Quantity{},
            std::nullopt,
            child.ordered)}));
        assert(seeded.cancel(ParentOrderId{40U}).accepted());
        first_cancel = poll_cancel(seeded);
        assert(child_for(seeded.inspect(), ParentOrderId{40U}).state ==
               ChildState::CancelPending);
    }

    {
        SorRuntime recovered{configuration(path), runtime_test_wire_factory()};
        const ChildSnapshot ambiguous = child_for(
            recovered.inspect(), ParentOrderId{40U});
        assert(ambiguous.state == ChildState::Ambiguous);
        publish_live(recovered);
        assert(recovered.publish(ReconciliationEvent{
            ambiguous.id,
            ReconciliationDisposition::DefinitelyUnsent,
            Quantity{},
        }));
        const CancelOrderIntent retry = poll_cancel(recovered);
        assert(retry.child == first_cancel.child);
        assert(retry.cancel_wire_order_id ==
               first_cancel.cancel_wire_order_id);
        dispatch_result(recovered, retry);
        assert(recovered.publish(ExecutionReportEvent{report(
            ambiguous,
            "CANCEL-TERMINAL",
            ExecutionKind::Canceled,
            NormalizedOrderStatus::Canceled,
            Quantity{},
            std::nullopt,
            Quantity{})}));
        assert(recovered.complete_recovery());
        const SorSnapshot terminal = recovered.inspect();
        assert(child_for(terminal, ParentOrderId{40U}).state ==
               ChildState::Canceled);
        assert(parent_for(terminal, ParentOrderId{40U}).state ==
               ParentState::Canceled);
    }
}

void crash_during_replace_and_replay_ids(
    const std::filesystem::path& path) {
    ChildSnapshot original;
    std::string cancel_wire_id;
    {
        SorRuntime seeded{configuration(path), runtime_test_wire_factory()};
        enable_empty_runtime(seeded);
        assert(seeded.submit(order(
            50U, RoutingIntent::PassiveGTC, 5U, 99)).accepted());
        const NewOrderIntent wire = poll_new(seeded);
        dispatch_result(seeded, wire);
        original = child_for(seeded.inspect(), ParentOrderId{50U});
        assert(seeded.publish(ExecutionReportEvent{report(
            original,
            "REPLACE-ACK",
            ExecutionKind::Acknowledged,
            NormalizedOrderStatus::Working,
            Quantity{},
            std::nullopt,
            original.ordered)}));
        assert(seeded.replace(ParentReplace{
            ParentOrderId{50U}, Quantity{8U}, Price{99U}}).accepted());
        const CancelOrderIntent pending = poll_cancel(seeded);
        cancel_wire_id = pending.cancel_wire_order_id;
        dispatch_result(seeded, pending);
        // The cancel is locally accepted but the venue terminal response is
        // absent when the process exits.
    }

    ChildOrderId replacement_id{};
    {
        SorRuntime recovered{configuration(path), runtime_test_wire_factory()};
        const ChildSnapshot before = child_for(
            recovered.inspect(), ParentOrderId{50U});
        assert(before.state == ChildState::CancelPending);
        publish_live(recovered);
        assert(recovered.publish(ReconciliationEvent{
            before.id,
            ReconciliationDisposition::Working,
            Quantity{},
        }));

        // Reconciliation proved the old child still works, so the pending
        // replacement must retry its durable cancel before routing revision 2.
        const CancelOrderIntent retry = poll_cancel(recovered);
        assert(retry.child == original.id);
        assert(retry.cancel_wire_order_id == cancel_wire_id);
        dispatch_result(recovered, retry);
        assert(recovered.publish(ExecutionReportEvent{report(
            before,
            "REPLACE-CANCELED",
            ExecutionKind::Canceled,
            NormalizedOrderStatus::Canceled,
            Quantity{},
            std::nullopt,
            Quantity{})}));

        const SorSnapshot planned = recovered.inspect();
        const ParentSnapshot& parent = parent_for(
            planned, ParentOrderId{50U});
        assert(parent.revision == 2U);
        assert(parent.requested_total == Quantity{8U});
        const ChildSnapshot& replacement = child_for(
            planned, ParentOrderId{50U}, 1U);
        replacement_id = replacement.id;
        assert(replacement_id.value() > original.id.value());
        assert(replacement.state == ChildState::Prepared);

        const NewOrderIntent replacement_wire = poll_new(recovered);
        assert(replacement_wire.child == replacement_id);
        dispatch_result(recovered, replacement_wire);
        assert(recovered.publish(ExecutionReportEvent{report(
            replacement,
            "REPLACEMENT-FILLED",
            ExecutionKind::Trade,
            NormalizedOrderStatus::Filled,
            replacement.ordered,
            replacement.ordered,
            Quantity{})}));
        assert(recovered.complete_recovery());
    }

    std::vector<ChildOrderId> first_replay_ids;
    {
        SorRuntime first_replay{configuration(path), runtime_test_wire_factory()};
        const SorSnapshot state = first_replay.inspect();
        for (const auto& child : state.children) {
            first_replay_ids.push_back(child.id);
        }
        assert(first_replay_ids.size() == 2U);
        assert(first_replay_ids[0] == original.id);
        assert(first_replay_ids[1] == replacement_id);
    }

    {
        SorRuntime second_replay{configuration(path), runtime_test_wire_factory()};
        const SorSnapshot state = second_replay.inspect();
        std::vector<ChildOrderId> second_replay_ids;
        for (const auto& child : state.children) {
            second_replay_ids.push_back(child.id);
        }
        assert(second_replay_ids == first_replay_ids);

        publish_live(second_replay);
        assert(second_replay.complete_recovery());
        assert(second_replay.submit(order(51U)).accepted());
        const ChildSnapshot next = child_for(
            second_replay.inspect(), ParentOrderId{51U});
        assert(next.id.value() > replacement_id.value());
        assert(std::find(
                   first_replay_ids.begin(),
                   first_replay_ids.end(),
                   next.id) == first_replay_ids.end());
    }
}

}  // namespace

int main() {
    const TempDirectory temporary;
    crash_before_commit_torn_tail(temporary.journal("before-commit"));
    crash_post_commit_pre_fix(temporary.journal("post-commit-pre-fix"));
    crash_post_fix_pre_ack(temporary.journal("post-fix-pre-ack"));
    crash_after_partial_fill(temporary.journal("after-partial"));
    crash_during_cancel(temporary.journal("during-cancel"));
    crash_during_replace_and_replay_ids(
        temporary.journal("during-replace"));
    return 0;
}

