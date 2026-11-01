#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/fastpath/dispatch_pipeline.hpp"
#include "sorx/recovery/fast_recovery.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> count{};

void record() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t size) {
    record();
    if (void* const memory = std::malloc(size == 0U ? 1U : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t size,
    const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* const memory =
            _aligned_malloc(size == 0U ? 1U : size, alignment)) {
        return memory;
    }
#else
    const std::size_t nonzero = size == 0U ? 1U : size;
    const std::size_t rounded =
        ((nonzero + alignment - 1U) / alignment) * alignment;
    if (void* const memory = std::aligned_alloc(alignment, rounded)) {
        return memory;
    }
#endif
    throw std::bad_alloc{};
}

void deallocate_aligned(void* const memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

class Scope final {
public:
    Scope() noexcept {
        count.store(0U, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    [[nodiscard]] std::size_t allocations() const noexcept {
        return count.load(std::memory_order_relaxed);
    }
};

}  // namespace allocation_probe

void* operator new(const std::size_t size) {
    return allocation_probe::allocate(size);
}
void* operator new[](const std::size_t size) {
    return allocation_probe::allocate(size);
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void* operator new(
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete(
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}

namespace {

using namespace sorx;
using namespace sorx::dataplane;
using namespace sorx::fastpath;
using namespace sorx::recovery;

constexpr VenueId kVenueOne{1U};
constexpr VenueId kVenueTwo{2U};
constexpr InstrumentId kInstrument{11U};
constexpr std::uint64_t kEpoch = 7U;
constexpr std::uint64_t kConfigurationGeneration = 13U;

enum class AdaptiveFixture : std::uint8_t {
    Unapplied,
    Eligible,
    FirstVenueExcluded,
};

[[nodiscard]] RouteConfig route(
    const VenueId venue,
    const std::uint32_t priority) {
    RouteConfig output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.venue_name = venue == kVenueOne ? "ONE" : "TWO";
    output.canonical_symbol = "TEST";
    output.wire_symbol = venue == kVenueOne ? "T1" : "T2";
    output.capabilities = VenueCapabilities{true, true};
    output.allocation_cap = Quantity{100U};
    output.aggressive_priority = priority;
    output.passive_priority = priority;
    output.configured_latency_ns = 100U + priority;
    return output;
}

[[nodiscard]] std::shared_ptr<const CandidateTable> make_candidates() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(
        std::vector<RouteConfig>{route(kVenueOne, 1U), route(kVenueTwo, 2U)},
        errors));
    FibCompiler compiler;
    return compiler.compile(rib);
}

[[nodiscard]] CoreConfiguration make_configuration() {
    CoreConfiguration output;
    output.maximum_aggressive_venues = 2U;
    output.risk.generation = 7U;
    InstrumentRiskLimit instrument;
    instrument.instrument = kInstrument;
    instrument.maximum_parent_quantity = Quantity{1'000U};
    instrument.maximum_child_quantity = Quantity{1'000U};
    instrument.maximum_parent_notional = Notional{1'000'000'000U};
    instrument.maximum_child_notional = Notional{1'000'000'000U};
    instrument.minimum_price = Price{1};
    instrument.maximum_price = Price{1'000'000};
    instrument.maximum_open_parents = 100U;
    instrument.maximum_open_children = 100U;
    instrument.maximum_gross_outstanding = Quantity{10'000U};
    output.risk.instruments.push_back(instrument);
    output.risk.venues.push_back(
        VenueRiskLimit{kVenueOne, true, Quantity{10'000U}});
    output.risk.venues.push_back(
        VenueRiskLimit{kVenueTwo, true, Quantity{10'000U}});
    return output;
}

[[nodiscard]] RouteKernel make_reference_kernel(
    const CoreConfiguration& configuration,
    const CandidateTable& candidates) {
    return RouteKernel{
        configuration,
        candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric};
}

[[nodiscard]] BookSlotView live_view(
    const VenueId venue,
    const Price ask,
    const Quantity ask_quantity) {
    md::BookView book;
    book.venue = venue;
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = kInstrument;
    assert(book.push_bid(
        md::Level{Price{ask.units - 2}, Quantity{10U}, 1U}));
    assert(book.push_ask(md::Level{ask, ask_quantity, 1U}));
    book.feed_session_epoch = 3U;
    book.recovery_generation = 1U;
    book.view_generation = 1U;
    book.last_applied_sequence = 1U;
    book.next_sequence = 2U;
    book.exchange_timestamp_ns = 900;
    book.local_receive_time_ns = 1'000U;
    book.local_publication_time_ns = 1'010U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.valid());

    BookSlotView output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.adjacency_generation = 9U;
    output.book = book;
    output.market_data_state = MarketDataState::Live;
    output.order_entry_state = OrderEntryState::Ready;
    output.egress_state = EgressState::Ready;
    output.instrument_state = InstrumentState::Trading;
    output.feed_epoch = book.feed_session_epoch;
    output.source_health_generation = 5U;
    output.monotonic_receive_time_ns = 1'000U;
    output.monotonic_publication_time_ns = 1'010U;
    output.maximum_quote_age_ns = 100U;
    output.tx_credit_generation = 4U;
    output.throttle_generation = 8U;
    output.normal_tx_credits = 10U;
    output.cancel_reserved_tx_credits = 2U;
    output.throttle_credits = 10U;
    output.has_book = true;
    output.source_healthy = true;
    output.conversion_compatible = true;
    output.risk_allowed = true;
    assert(output.valid());
    return output;
}

[[nodiscard]] std::array<BookSlotView, 2U> views() {
    return {
        live_view(kVenueOne, Price{100}, Quantity{4U}),
        live_view(kVenueTwo, Price{101}, Quantity{6U}),
    };
}

[[nodiscard]] ParentOrder parent(const std::uint64_t id) noexcept {
    return ParentOrder{
        ParentOrderId{id},
        kInstrument,
        Side::Buy,
        Quantity{10U},
        Price{101},
        RoutingIntent::AggressiveIOC,
    };
}

[[nodiscard]] policy::PolicyVersionPin policy_versions() noexcept {
    return policy::PolicyVersionPin{
        10U,
        20U,
        30U,
        40U,
        50U,
        611U,
        3U,
        100U,
        0U,
    };
}

[[nodiscard]] policy::VenueFactPin fact_pin(
    const BookSlotView& view) noexcept {
    return policy::VenueFactPin{
        view.venue,
        policy::VenueFactVersions{
            view.adjacency_generation,
            view.book.view_generation,
            1U,
            7U,
            view.throttle_generation,
            view.tx_credit_generation,
            1U,
            20U,
            30U,
        },
    };
}

[[nodiscard]] AdaptivePolicyOverlay eligible_overlay(
    const ParentOrder& order,
    const BookSlotView& view,
    const std::uint64_t decision_time_ns,
    const std::uint16_t rank) noexcept {
    AdaptivePolicyOverlay output;
    output.versions = policy_versions();
    output.order = policy::PolicyOrder{
        order.id,
        order.instrument,
        order.side,
        order.intent,
        order.quantity,
        order.limit_price,
        decision_time_ns,
        500U,
    };
    output.fact_pin = fact_pin(view);
    output.evidence = policy::RankedCandidate{
        view.venue,
        output.fact_pin,
        view.book.asks[0U].price,
        view.book.asks[0U].quantity,
        decision_time_ns - view.monotonic_receive_time_ns,
        view.maximum_quote_age_ns,
        view.throttle_credits,
        view.normal_tx_credits,
        policy::ProtectedQuoteFacts{true, true, Price{100}},
        0,
        policy::BoundedVenueMetrics{0U, 0U, 0U, 100U},
        policy::ScoreBreakdown{},
        view.venue == kVenueOne ? 1U : 2U,
    };
    output.exclusion_reason = policy::PolicyExclusionReason::None;
    output.rank = rank;
    output.applied = true;
    output.eligible = true;
    assert(output.valid_shape());
    return output;
}

[[nodiscard]] AdaptivePolicyOverlay excluded_overlay(
    const ParentOrder& order,
    const BookSlotView& view,
    const std::uint64_t decision_time_ns) noexcept {
    AdaptivePolicyOverlay output;
    output.versions = policy_versions();
    output.order = policy::PolicyOrder{
        order.id,
        order.instrument,
        order.side,
        order.intent,
        order.quantity,
        order.limit_price,
        decision_time_ns,
        500U,
    };
    output.fact_pin = fact_pin(view);
    output.exclusion_reason = policy::PolicyExclusionReason::RiskDenied;
    output.applied = true;
    assert(output.valid_shape());
    return output;
}

void apply_adaptive_fixture(
    const AdaptiveFixture fixture,
    const ParentOrder& order,
    const std::uint64_t decision_time_ns,
    std::array<BookSlotView, 2U>& pinned) noexcept {
    if (fixture == AdaptiveFixture::Unapplied) {
        return;
    }
    if (fixture == AdaptiveFixture::Eligible) {
        pinned[0U].adaptive_policy =
            eligible_overlay(order, pinned[0U], decision_time_ns, 0U);
        pinned[1U].adaptive_policy =
            eligible_overlay(order, pinned[1U], decision_time_ns, 1U);
        return;
    }
    pinned[0U].adaptive_policy =
        excluded_overlay(order, pinned[0U], decision_time_ns);
    pinned[1U].adaptive_policy =
        eligible_overlay(order, pinned[1U], decision_time_ns, 0U);
}

void put_u64(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index{}; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        output[offset + index] = static_cast<std::byte>(value >> shift);
    }
}

class Encoder final : public IChildFrameEncoder {
public:
    [[nodiscard]] FrameEncodeResult encode_new(
        const ParentOrder& order,
        const ChildAllocation& allocation,
        const PlannedChild& child,
        const std::span<std::byte> destination) noexcept override {
        constexpr std::size_t kHeaderBytes = 32U;
        const std::size_t required = kHeaderBytes + child.wire_identity.size;
        if (required > destination.size() || !order.id || !allocation.venue ||
            !child.child_order_id || child.wire_identity.size == 0U) {
            return {FrameEncodeStatus::InvalidInput, 0U};
        }
        put_u64(destination, 0U, order.id.value());
        put_u64(destination, 8U, child.child_order_id.value());
        put_u64(destination, 16U, child.dispatch_attempt_id);
        put_u64(destination, 24U, allocation.quantity.units);
        std::copy_n(
            child.wire_identity.bytes.begin(),
            child.wire_identity.size,
            destination.begin() + kHeaderBytes);
        return {
            FrameEncodeStatus::Encoded,
            static_cast<std::uint16_t>(required),
        };
    }
};

class MutatingRecorder final : public recorder::IFlightRecorder {
public:
    MutatingRecorder(
        recorder::IFlightRecorder& inner,
        RouteKernel& kernel) noexcept
        : inner_(&inner), kernel_(&kernel) {}

    [[nodiscard]] recorder::AppendStatus try_append(
        const recorder::DurableBatch& batch,
        recorder::CompletionSlot& completion) noexcept override {
        const recorder::AppendStatus status =
            inner_->try_append(batch, completion);
        if (status == recorder::AppendStatus::Committed) {
            const bool changed = kernel_->set_kill_switch(true);
            assert(changed);
        }
        return status;
    }
    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableToken& token,
        const recorder::DurabilityDomain domain) const noexcept override {
        return inner_->authorizes_external_effect(token, domain);
    }
    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableEffectPermit& permit,
        const recorder::DurabilityDomain domain) const noexcept override {
        return inner_->authorizes_external_effect(permit, domain);
    }
    [[nodiscard]] recorder::ReadStatus read(
        const std::uint64_t id,
        recorder::DurableBatch& batch,
        recorder::DurableToken& token) const noexcept override {
        return inner_->read(id, batch, token);
    }
    [[nodiscard]] recorder::ReplayStatus replay(
        const recorder::ReplayRange range,
        recorder::IReplaySink& sink) const noexcept override {
        return inner_->replay(range, sink);
    }
    [[nodiscard]] recorder::AcknowledgeStatus acknowledge(
        const std::uint64_t id) noexcept override {
        return inner_->acknowledge(id);
    }
    [[nodiscard]] recorder::DrainStatus drain_to_tier1(
        const recorder::ReplayRange range,
        recorder::ITier1SegmentSink& sink) noexcept override {
        return inner_->drain_to_tier1(range, sink);
    }
    [[nodiscard]] recorder::FlightRecorderMetadata metadata()
        const noexcept override {
        return inner_->metadata();
    }
    [[nodiscard]] recorder::DurabilityDomain durability_domain()
        const noexcept override {
        return inner_->durability_domain();
    }

private:
    recorder::IFlightRecorder* inner_{};
    RouteKernel* kernel_{};
};

struct CanonicalLog final {
    CanonicalLog()
        : candidates(make_candidates()),
          configuration(make_configuration()),
          kernel(
              configuration,
              *candidates,
              AdaptivePolicyMode::Optional,
              WireIdentityMode::ReferenceGeneric),
          recorder({32U, 4U, 4U, false}),
          pipeline(
              kernel,
              recorder,
              registry,
              DispatchPipelinePolicy{
                  kEpoch,
                  kConfigurationGeneration,
                  recorder::DurabilityDomain::VolatileSoftwareReference}) {
        assert(registry.register_venue(kVenueOne, queue_one, encoder_one) ==
               VenueRegistrationStatus::Registered);
        assert(registry.register_venue(kVenueTwo, queue_two, encoder_two) ==
               VenueRegistrationStatus::Registered);
        assert(registry.seal());
    }

    [[nodiscard]] DispatchPipelineResult dispatch(
        const std::uint64_t parent_id,
        const std::uint64_t producer_sequence,
        const AdaptiveFixture adaptive_fixture =
            AdaptiveFixture::Unapplied) noexcept {
        auto pinned = views();
        const ParentOrder command = parent(parent_id);
        const std::uint64_t decision_time_ns =
            1'050U + producer_sequence;
        apply_adaptive_fixture(
            adaptive_fixture, command, decision_time_ns, pinned);
        return pipeline.try_dispatch(
            command,
            pinned,
            DispatchRequestMetadata{
                producer_sequence,
                decision_time_ns,
                1'060U + producer_sequence,
                1'055U + producer_sequence,
            });
    }

    std::shared_ptr<const CandidateTable> candidates;
    CoreConfiguration configuration;
    RouteKernel kernel;
    recorder::InMemoryMirroredTier0 recorder;
    VenueTxQueues queue_one;
    VenueTxQueues queue_two;
    Encoder encoder_one;
    Encoder encoder_two;
    FixedVenueRegistry registry;
    DispatchPipeline pipeline;
};

[[nodiscard]] RecoveryConfiguration recovery_configuration(
    const CandidateTable& candidates,
    const std::uint64_t configuration_generation =
        kConfigurationGeneration) noexcept {
    RecoveryConfiguration output;
    output.active_parent_capacity = 16U;
    output.client_capacity = 8U;
    output.client_dedupe_capacity = 32U;
    output.replay_dedupe_capacity = 16U;
    output.leadership_epoch = kEpoch;
    output.configuration_generation = configuration_generation;
    output.candidate_table_generation = candidates.generation();
    output.risk_generation = 7U;
    return output;
}

[[nodiscard]] RecoveryRequestDigest digest(const std::uint8_t seed) noexcept {
    RecoveryRequestDigest output{};
    for (std::size_t index{}; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>(seed + index);
    }
    return output;
}

[[nodiscard]] RecoveryRecord record(
    const RecoveryRecordType type,
    const std::uint64_t source_sequence,
    const std::uint64_t client_sequence,
    const std::uint8_t digest_seed,
    const std::uint64_t parent_id,
    const std::uint64_t child_id,
    const std::uint64_t attempt,
    const Quantity quantity = Quantity{}) noexcept {
    RecoveryRecord output{
        type,
        source_sequence,
        99U,
        client_sequence,
        digest(digest_seed),
        ParentOrderId{parent_id},
        ChildOrderId{child_id},
        attempt,
        quantity,
    };
    if (type == RecoveryRecordType::CancelIntent) {
        output.prior_dispatch_attempt_id = attempt;
        output.dispatch_attempt_id = attempt + 100U;
        output.client_request_id = source_sequence;
        output.client_session_epoch = 1U;
    }
    return output;
}

void reauthorize_modified_batch(
    const recorder::DurableBatch& source,
    const std::size_t modified_record,
    const std::size_t modified_payload_offset,
    const bool change_type,
    recorder::DurableBatch& output_batch,
    recorder::DurableToken& output_token) {
    recorder::DurableBatch forged(
        source.lane(), source.producer_sequence());
    forged.set_leadership_epoch(source.leadership_epoch());
    forged.set_configuration_generation(source.configuration_generation());
    assert(forged.set_authorization_envelope_version(
        source.authorization_envelope_version()));
    assert(forged.set_dispatch_authorization(
        source.dispatch_authorization()));
    for (const recorder::ExternalEffectAuthorization& authorization :
         source.external_effect_authorizations().values()) {
        assert(forged.try_add_external_effect_authorization(authorization));
    }
    std::array<std::byte, recorder::kMaximumDurableBatchBytes> payload{};
    for (std::size_t index{}; index < source.record_count(); ++index) {
        const auto record_view = source.record(index);
        assert(record_view.has_value());
        std::span<const std::byte> bytes = record_view->payload;
        if (index == modified_record) {
            assert(bytes.size() <= payload.size());
            assert(modified_payload_offset < bytes.size());
            std::copy(bytes.begin(), bytes.end(), payload.begin());
            payload[modified_payload_offset] ^= std::byte{1U};
            bytes = {payload.data(), bytes.size()};
        }
        const std::uint16_t type = static_cast<std::uint16_t>(
            record_view->type +
            (change_type && index == modified_record ? 1U : 0U));
        assert(forged.try_append_record(
            type, record_view->version, bytes));
    }
    recorder::InMemoryMirroredTier0 tier0({8U, 1U, 1U, false});
    recorder::CompletionSlot completion;
    assert(tier0.try_append(forged, completion) ==
           recorder::AppendStatus::Committed);
    assert(tier0.read(1U, output_batch, output_token) ==
           recorder::ReadStatus::Ok);
}

void reauthorize_replaced_record(
    const recorder::DurableBatch& source,
    const std::size_t modified_record,
    const std::span<const std::byte> replacement_payload,
    const std::uint16_t record_version,
    recorder::DurableBatch& output_batch,
    recorder::DurableToken& output_token) {
    assert(modified_record < source.record_count());
    recorder::DurableBatch forged(
        source.lane(), source.producer_sequence());
    forged.set_leadership_epoch(source.leadership_epoch());
    forged.set_configuration_generation(source.configuration_generation());
    assert(forged.set_authorization_envelope_version(
        source.authorization_envelope_version()));
    assert(forged.set_dispatch_authorization(
        source.dispatch_authorization()));
    for (const recorder::ExternalEffectAuthorization& authorization :
         source.external_effect_authorizations().values()) {
        assert(forged.try_add_external_effect_authorization(authorization));
    }
    for (std::size_t index{}; index < source.record_count(); ++index) {
        const auto record_view = source.record(index);
        assert(record_view.has_value());
        const std::span<const std::byte> payload = index == modified_record
            ? replacement_payload
            : record_view->payload;
        const std::uint16_t version = record_version == 0U
            ? record_view->version
            : record_version;
        assert(forged.try_append_record(
            record_view->type, version, payload));
    }
    recorder::InMemoryMirroredTier0 tier0({8U, 1U, 1U, false});
    recorder::CompletionSlot completion;
    assert(tier0.try_append(forged, completion) ==
           recorder::AppendStatus::Committed);
    assert(tier0.read(1U, output_batch, output_token) ==
           recorder::ReadStatus::Ok);
}

[[nodiscard]] std::uint16_t read_test_u16(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept {
    assert(offset + 2U <= input.size());
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[offset]) << 8U) |
        std::to_integer<std::uint16_t>(input[offset + 1U]));
}

[[nodiscard]] std::size_t legacy_frame_prefix_size(
    const std::span<const std::byte> payload) noexcept {
    if (payload.size() < 2U) {
        return 0U;
    }
    const std::uint16_t count = read_test_u16(payload, 0U);
    std::size_t offset = 2U;
    for (std::size_t index{}; index < count; ++index) {
        constexpr std::size_t kFixedThroughDispatchClass =
            4U + (4U * 8U) + 1U;
        if (payload.size() - offset <
            kFixedThroughDispatchClass + 1U) {
            return 0U;
        }
        offset += kFixedThroughDispatchClass;
        const std::size_t wire_size =
            std::to_integer<std::uint8_t>(payload[offset++]);
        if (wire_size > dataplane::kInlineWireIdentityBytes ||
            payload.size() - offset < wire_size + 2U) {
            return 0U;
        }
        offset += wire_size;
        const std::size_t frame_size = read_test_u16(payload, offset);
        offset += 2U;
        if (frame_size == 0U ||
            frame_size > fastpath::kMaximumEncodedFrameBytes ||
            payload.size() - offset < frame_size) {
            return 0U;
        }
        offset += frame_size;
    }
    return offset;
}

[[nodiscard]] bool make_legacy_unapplied_pinned_payload(
    const std::span<const std::byte> input,
    const std::span<std::byte> output,
    std::size_t& output_size) noexcept {
    std::size_t source_offset{};
    std::size_t destination_offset{};
    const auto copy_bytes = [&](const std::size_t count) noexcept {
        if (count > input.size() - source_offset ||
            count > output.size() - destination_offset) {
            return false;
        }
        std::copy_n(
            input.begin() + source_offset,
            count,
            output.begin() + destination_offset);
        source_offset += count;
        destination_offset += count;
        return true;
    };
    if (input.size() < 2U) {
        return false;
    }
    const std::uint16_t pin_count = read_test_u16(input, 0U);
    if (pin_count == 0U || !copy_bytes(2U)) {
        return false;
    }
    for (std::size_t index{}; index < pin_count; ++index) {
        if (!copy_bytes(12U) || source_offset >= input.size()) {
            return false;
        }
        const std::uint8_t has_version =
            std::to_integer<std::uint8_t>(input[source_offset]);
        if (has_version > 1U || !copy_bytes(1U) ||
            (has_version != 0U && !copy_bytes(48U)) ||
            !copy_bytes(44U) || source_offset >= input.size() ||
            input[source_offset] != std::byte{}) {
            return false;
        }
        ++source_offset;
        if (source_offset >= input.size()) {
            return false;
        }
        const std::uint8_t view_present =
            std::to_integer<std::uint8_t>(input[source_offset]);
        if (view_present > 1U || !copy_bytes(1U)) {
            return false;
        }
        if (view_present == 0U) {
            continue;
        }
        if (input.size() - source_offset < 92U) {
            return false;
        }
        const std::uint8_t has_book =
            std::to_integer<std::uint8_t>(input[source_offset + 88U]);
        if (has_book > 1U || !copy_bytes(92U) ||
            (has_book != 0U && !copy_bytes(17U))) {
            return false;
        }
    }
    if (!copy_bytes(input.size() - source_offset)) {
        return false;
    }
    output_size = destination_offset;
    return true;
}

void test_durable_before_kernel_commit() {
    const auto candidates = make_candidates();
    const CoreConfiguration configuration = make_configuration();
    RouteKernel source_kernel =
        make_reference_kernel(configuration, *candidates);
    recorder::InMemoryMirroredTier0 tier0({16U, 2U, 2U, false});
    MutatingRecorder recorder(tier0, source_kernel);
    VenueTxQueues queue_one;
    VenueTxQueues queue_two;
    Encoder encoder_one;
    Encoder encoder_two;
    FixedVenueRegistry registry;
    assert(registry.register_venue(kVenueOne, queue_one, encoder_one) ==
           VenueRegistrationStatus::Registered);
    assert(registry.register_venue(kVenueTwo, queue_two, encoder_two) ==
           VenueRegistrationStatus::Registered);
    assert(registry.seal());
    DispatchPipeline pipeline(
        source_kernel,
        recorder,
        registry,
        DispatchPipelinePolicy{
            kEpoch,
            kConfigurationGeneration,
            recorder::DurabilityDomain::VolatileSoftwareReference});
    const auto pinned = views();
    const DispatchPipelineResult dispatched = pipeline.try_dispatch(
        parent(1U),
        pinned,
        DispatchRequestMetadata{1U, 1'051U, 1'061U, 1'056U});
    assert(dispatched.status == DispatchPipelineStatus::KernelCommitRejected);
    assert(source_kernel.active_parent_count() == 0U);

    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(tier0.read(1U, batch, token) == recorder::ReadStatus::Ok);
    RouteKernel recovered_kernel =
        make_reference_kernel(configuration, *candidates);
    RecoveryLedger ledger(
        recovered_kernel, recovery_configuration(*candidates));
    const DispatchReplayResult replay =
        ledger.replay_dispatch_batch(token, batch);
    assert(replay.status == DispatchReplayStatus::Applied);
    assert(recovered_kernel.active_parent_count() == 1U);
    assert(recovered_kernel.recovery_only());
    assert(recovered_kernel.unreconciled_child_count() == 2U);
}

void test_after_commit_before_complete_enqueue_boundary() {
    const auto candidates = make_candidates();
    const CoreConfiguration configuration = make_configuration();
    RouteKernel source_kernel =
        make_reference_kernel(configuration, *candidates);
    recorder::InMemoryMirroredTier0 tier0({16U, 2U, 2U, false});
    VenueTxQueues shared_queue;
    Encoder encoder_one;
    Encoder encoder_two;
    FixedVenueRegistry registry;
    assert(registry.register_venue(kVenueOne, shared_queue, encoder_one) ==
           VenueRegistrationStatus::Registered);
    assert(registry.register_venue(kVenueTwo, shared_queue, encoder_two) ==
           VenueRegistrationStatus::Registered);
    assert(registry.seal());

    EncodedFrame filler;
    filler.venue_id = kVenueOne.value();
    filler.child_id = 1'000U;
    filler.dispatch_attempt_id = 2'000U;
    filler.leadership_epoch = kEpoch;
    filler.configuration_generation = kConfigurationGeneration;
    filler.venue_session_id = 1U;
    filler.venue_session_epoch = 1U;
    filler.ownership_generation = 1U;
    filler.dispatch_class = DispatchClass::NewOrReplace;
    filler.size = 1U;
    filler.storage[0] = std::byte{0x55U};
    recorder::DurableBatch filler_batch(
        recorder::DurabilityLane::Normal, 99U);
    filler_batch.set_leadership_epoch(kEpoch);
    filler_batch.set_configuration_generation(kConfigurationGeneration);
    assert(filler_batch.set_dispatch_authorization({1'000U, 2'000U, 1U}));
    assert(filler_batch.try_add_external_effect_authorization(
        filler.venue_id,
        filler.child_id,
        filler.dispatch_attempt_id,
        filler.venue_session_id,
        filler.venue_session_epoch,
        filler.ownership_generation,
        static_cast<std::uint8_t>(filler.dispatch_class),
        filler.bytes()));
    const std::array filler_payload{std::byte{0x55U}};
    assert(filler_batch.try_append_record(1U, 1U, filler_payload));
    recorder::InMemoryMirroredTier0 filler_recorder({8U, 1U, 1U, false});
    recorder::CompletionSlot filler_completion;
    assert(filler_recorder.try_append(filler_batch, filler_completion) ==
           recorder::AppendStatus::Committed);
    const auto filler_token = filler_completion.token();
    assert(filler_token.has_value());
    const auto filler_permit = filler_token->effect_permit(0U);
    assert(filler_permit.has_value());
    for (std::uint64_t index{}; index < 63U; ++index) {
        static_cast<void>(index);
        assert(shared_queue.try_enqueue_normal(filler, *filler_permit));
    }
    assert(shared_queue.normal_available_approx() == 1U);

    DispatchPipeline pipeline(
        source_kernel,
        tier0,
        registry,
        DispatchPipelinePolicy{
            kEpoch,
            kConfigurationGeneration,
            recorder::DurabilityDomain::VolatileSoftwareReference});
    const auto pinned = views();
    const DispatchPipelineResult dispatched = pipeline.try_dispatch(
        parent(1U),
        pinned,
        DispatchRequestMetadata{1U, 1'051U, 1'061U, 1'056U});
    assert(dispatched.status ==
           DispatchPipelineStatus::QueueOwnershipInvariantViolated);
    assert(dispatched.commit_status == CommitStatus::Committed);
    assert(source_kernel.active_parent_count() == 1U);

    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(tier0.read(1U, batch, token) == recorder::ReadStatus::Ok);
    RouteKernel recovered_kernel =
        make_reference_kernel(configuration, *candidates);
    RecoveryLedger ledger(
        recovered_kernel, recovery_configuration(*candidates));
    assert(ledger.replay_dispatch_batch(token, batch).status ==
           DispatchReplayStatus::Applied);
    assert(recovered_kernel.unreconciled_child_count() == 2U);
}

void test_commit_boundary_idempotency_integrity_and_allocations() {
    CanonicalLog log;
    assert(log.dispatch(1U, 1U).dispatched());
    assert(log.dispatch(2U, 2U).dispatched());
    recorder::DurableBatch first_batch;
    recorder::DurableToken first_token;
    recorder::DurableBatch second_batch;
    recorder::DurableToken second_token;
    assert(log.recorder.read(1U, first_batch, first_token) ==
           recorder::ReadStatus::Ok);
    assert(log.recorder.read(2U, second_batch, second_token) ==
           recorder::ReadStatus::Ok);

    RouteKernel kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger ledger(kernel, recovery_configuration(*log.candidates));
    {
        allocation_probe::Scope no_allocations;
        assert(ledger.replay_dispatch_batch(first_token, first_batch).status ==
               DispatchReplayStatus::Applied);
        assert(no_allocations.allocations() == 0U);
    }
    const InstrumentRiskCounters before =
        kernel.instrument_counters(kInstrument);
    assert(ledger.replay_dispatch_batch(first_token, first_batch).status ==
           DispatchReplayStatus::AlreadyApplied);
    assert(kernel.instrument_counters(kInstrument).gross_outstanding ==
           before.gross_outstanding);
    assert(ledger.replay_dispatch_batch(second_token, second_batch).status ==
           DispatchReplayStatus::Applied);
    assert(kernel.active_parent_count() == 2U);
    assert(kernel.recovery_identifiers().next_child_order_id == 5U);
    assert(kernel.recovery_identifiers().next_dispatch_attempt_id == 5U);

    recorder::DurableToken tampered = second_token;
    tampered.batch_hash[0] ^= std::byte{1U};
    RouteKernel tamper_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger tamper_ledger(
        tamper_kernel, recovery_configuration(*log.candidates));
    assert(tamper_ledger.replay_dispatch_batch(tampered, second_batch).status ==
           DispatchReplayStatus::IntegrityFailure);

    const auto empty_views = std::span<const BookSlotView>{};
    assert(kernel.plan(parent(3U), empty_views, 2'000U).status ==
           PlanStatus::RecoveryOnly);
    assert(!kernel.set_routing_enabled(true));
    assert(!kernel.set_kill_switch(false));
}

void test_partial_tx_ambiguity_and_terminal_retirement() {
    CanonicalLog log;
    assert(log.dispatch(1U, 1U).dispatched());
    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(log.recorder.read(1U, batch, token) == recorder::ReadStatus::Ok);
    RouteKernel kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger ledger(kernel, recovery_configuration(*log.candidates));
    assert(ledger.replay_dispatch_batch(token, batch).status ==
           DispatchReplayStatus::Applied);
    assert(!ledger.complete_reconciliation());

    const RecoveryRecord working = record(
        RecoveryRecordType::ReconcileWorking,
        1U,
        1U,
        1U,
        1U,
        1U,
        1U);
    assert(ledger.apply_record(working).status ==
           RecoveryRecordStatus::Applied);
    assert(ledger.apply_record(working).status ==
           RecoveryRecordStatus::DuplicateReplay);
    RecoveryRecord conflict = working;
    conflict.request_digest = digest(2U);
    assert(ledger.apply_record(conflict).status ==
           RecoveryRecordStatus::DuplicateConflict);
    assert(!ledger.complete_reconciliation());
    assert(kernel.instrument_counters(kInstrument).gross_outstanding ==
           Quantity{10U});

    assert(ledger.apply_record(record(
        RecoveryRecordType::Fill,
        2U,
        2U,
        3U,
        1U,
        2U,
        2U,
        Quantity{2U})).status == RecoveryRecordStatus::Applied);
    assert(kernel.instrument_counters(kInstrument).gross_outstanding ==
           Quantity{8U});
    assert(!ledger.complete_reconciliation());

    assert(ledger.apply_record(record(
        RecoveryRecordType::ReconcileTerminal,
        3U,
        3U,
        4U,
        1U,
        2U,
        2U)).status == RecoveryRecordStatus::Applied);
    assert(ledger.complete_reconciliation());
    assert(!kernel.recovery_only());
    assert(kernel.kill_switch());
    assert(!kernel.routing_enabled());
    assert(kernel.active_parent_count() == 1U);

    assert(kernel.set_kill_switch(false));
    assert(kernel.set_routing_enabled(true));
    assert(kernel.release_child(ParentOrderId{1U}, kVenueOne) ==
           ReleaseStatus::Released);
    assert(kernel.active_parent_count() == 0U);
    assert(kernel.instrument_counters(kInstrument).gross_outstanding ==
           Quantity{});
}

void test_canonical_record_cross_validation() {
    CanonicalLog log;
    assert(log.dispatch(1U, 1U).dispatched());
    recorder::DurableBatch source;
    recorder::DurableToken source_token;
    assert(log.recorder.read(1U, source, source_token) ==
           recorder::ReadStatus::Ok);

    // ChildDispatches layout begins with count (2), then venue (4), child (8),
    // attempt (8), and frame leadership epoch. The forged batch receives a
    // fresh valid recorder hash/token, so rejection is canonical cross-field
    // validation rather than a checksum shortcut.
    recorder::DurableBatch bad_frame;
    recorder::DurableToken bad_frame_token;
    reauthorize_modified_batch(
        source, 3U, 22U, false, bad_frame, bad_frame_token);
    RouteKernel frame_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger frame_ledger(
        frame_kernel, recovery_configuration(*log.candidates));
    assert(frame_ledger.replay_dispatch_batch(
        bad_frame_token, bad_frame).status ==
        DispatchReplayStatus::CanonicalDecodeFailed);
    assert(frame_kernel.active_parent_count() == 0U);

    recorder::DurableBatch bad_generation;
    recorder::DurableToken bad_generation_token;
    reauthorize_modified_batch(
        source, 2U, 23U, false, bad_generation, bad_generation_token);
    RouteKernel generation_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger generation_ledger(
        generation_kernel, recovery_configuration(*log.candidates));
    assert(generation_ledger.replay_dispatch_batch(
        bad_generation_token, bad_generation).status ==
        DispatchReplayStatus::GenerationMismatch);

    recorder::DurableBatch wrong_type;
    recorder::DurableToken wrong_type_token;
    reauthorize_modified_batch(source, 0U, 0U, true, wrong_type, wrong_type_token);
    RouteKernel type_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger type_ledger(
        type_kernel, recovery_configuration(*log.candidates));
    assert(type_ledger.replay_dispatch_batch(
        wrong_type_token, wrong_type).status ==
        DispatchReplayStatus::InvalidRecordSet);
}

void test_dispatch_v3_adaptive_schema_and_legacy_v1() {
    CanonicalLog unapplied_log;
    assert(unapplied_log.dispatch(
        1U, 1U, AdaptiveFixture::Unapplied).dispatched());
    recorder::DurableBatch unapplied_batch;
    recorder::DurableToken unapplied_token;
    assert(unapplied_log.recorder.read(
        1U, unapplied_batch, unapplied_token) == recorder::ReadStatus::Ok);
    const auto unapplied_pins = unapplied_batch.record(1U);
    assert(unapplied_pins.has_value());
    assert(unapplied_pins->version == kDispatchRecordVersion);
    RouteKernel unapplied_kernel = make_reference_kernel(
        unapplied_log.configuration, *unapplied_log.candidates);
    RecoveryLedger unapplied_ledger(
        unapplied_kernel,
        recovery_configuration(*unapplied_log.candidates));
    assert(unapplied_ledger.replay_dispatch_batch(
        unapplied_token, unapplied_batch).status ==
        DispatchReplayStatus::Applied);

    CanonicalLog eligible_log;
    assert(eligible_log.dispatch(
        1U, 1U, AdaptiveFixture::Eligible).dispatched());
    recorder::DurableBatch eligible_batch;
    recorder::DurableToken eligible_token;
    assert(eligible_log.recorder.read(
        1U, eligible_batch, eligible_token) == recorder::ReadStatus::Ok);
    RouteKernel eligible_kernel = make_reference_kernel(
        eligible_log.configuration, *eligible_log.candidates);
    RecoveryLedger eligible_ledger(
        eligible_kernel,
        recovery_configuration(*eligible_log.candidates));
    {
        allocation_probe::Scope no_allocations;
        const DispatchReplayResult replay =
            eligible_ledger.replay_dispatch_batch(
                eligible_token, eligible_batch);
        assert(replay.status == DispatchReplayStatus::Applied);
        assert(replay.child_count == 2U);
        assert(no_allocations.allocations() == 0U);
    }

    CanonicalLog excluded_log;
    assert(excluded_log.dispatch(
        1U, 1U, AdaptiveFixture::FirstVenueExcluded).dispatched());
    recorder::DurableBatch excluded_batch;
    recorder::DurableToken excluded_token;
    assert(excluded_log.recorder.read(
        1U, excluded_batch, excluded_token) == recorder::ReadStatus::Ok);
    RouteKernel excluded_kernel = make_reference_kernel(
        excluded_log.configuration, *excluded_log.candidates);
    RecoveryLedger excluded_ledger(
        excluded_kernel,
        recovery_configuration(*excluded_log.candidates));
    {
        allocation_probe::Scope no_allocations;
        const DispatchReplayResult replay =
            excluded_ledger.replay_dispatch_batch(
                excluded_token, excluded_batch);
        assert(replay.status == DispatchReplayStatus::Applied);
        assert(replay.child_count == 1U);
        assert(no_allocations.allocations() == 0U);
    }

    const auto eligible_pins = eligible_batch.record(1U);
    assert(eligible_pins.has_value());
    const std::span<const std::byte> canonical = eligible_pins->payload;
    // First pin, with a present BookVersion: applied=107, eligible=108,
    // rank=109, policy-order parent=184, and score.saturated=440.
    assert(canonical.size() > 441U);
    const auto expect_rejection = [&](
        const recorder::DurableBatch& forged,
        const recorder::DurableToken& token,
        const DispatchReplayStatus expected) {
        RouteKernel kernel = make_reference_kernel(
            eligible_log.configuration, *eligible_log.candidates);
        RecoveryLedger ledger(
            kernel, recovery_configuration(*eligible_log.candidates));
        assert(ledger.replay_dispatch_batch(token, forged).status == expected);
        assert(kernel.active_parent_count() == 0U);
    };
    const auto forge_payload = [&](
        const std::span<const std::byte> replacement,
        const std::uint16_t version,
        const DispatchReplayStatus expected) {
        recorder::DurableBatch forged;
        recorder::DurableToken token;
        reauthorize_replaced_record(
            eligible_batch, 1U, replacement, version, forged, token);
        expect_rejection(forged, token, expected);
    };

    std::array<std::byte, recorder::kMaximumDurableBatchBytes> modified{};
    std::copy(canonical.begin(), canonical.end(), modified.begin());
    modified[107U] = std::byte{0x02U};
    forge_payload(
        {modified.data(), canonical.size()},
        0U,
        DispatchReplayStatus::CanonicalDecodeFailed);

    std::copy(canonical.begin(), canonical.end(), modified.begin());
    modified[191U] ^= std::byte{0x01U};
    forge_payload(
        {modified.data(), canonical.size()},
        0U,
        DispatchReplayStatus::CanonicalDecodeFailed);

    std::copy(canonical.begin(), canonical.end(), modified.begin());
    modified[440U] = std::byte{0x01U};
    forge_payload(
        {modified.data(), canonical.size()},
        0U,
        DispatchReplayStatus::CanonicalDecodeFailed);

    forge_payload(
        canonical.first(canonical.size() - 1U),
        0U,
        DispatchReplayStatus::CanonicalDecodeFailed);

    std::copy(canonical.begin(), canonical.end(), modified.begin());
    modified[canonical.size()] = std::byte{0xa5U};
    forge_payload(
        {modified.data(), canonical.size() + 1U},
        0U,
        DispatchReplayStatus::CanonicalDecodeFailed);

    forge_payload(
        canonical,
        4U,
        DispatchReplayStatus::InvalidRecordSet);

    std::array<std::byte, recorder::kMaximumDurableBatchBytes> legacy_payload{};
    std::size_t legacy_size{};
    assert(make_legacy_unapplied_pinned_payload(
        unapplied_pins->payload, legacy_payload, legacy_size));
    assert(legacy_size + 2U == unapplied_pins->payload.size());
    const auto make_compatibility_batch = [](
        const recorder::DurableBatch& source,
        const std::span<const std::byte> pinned_payload,
        const std::uint16_t version,
        recorder::DurableBatch& output_batch,
        recorder::DurableToken& output_token) {
        recorder::DurableBatch forged(
            source.lane(), source.producer_sequence());
        forged.set_leadership_epoch(source.leadership_epoch());
        forged.set_configuration_generation(
            source.configuration_generation());
        assert(forged.set_authorization_envelope_version(
            recorder::AuthorizationEnvelopeVersion::LegacyRangeV0));
        assert(forged.set_dispatch_authorization(
            source.dispatch_authorization()));
        for (std::size_t index{}; index < source.record_count(); ++index) {
            const auto record_view = source.record(index);
            assert(record_view.has_value());
            std::span<const std::byte> payload = record_view->payload;
            if (index == 1U) {
                payload = pinned_payload;
            } else if (index == 3U) {
                const std::size_t prefix = legacy_frame_prefix_size(payload);
                assert(prefix != 0U && prefix < payload.size());
                payload = payload.first(prefix);
            }
            assert(forged.try_append_record(
                record_view->type, version, payload));
        }
        recorder::InMemoryMirroredTier0 tier0({8U, 1U, 1U, false});
        recorder::CompletionSlot completion;
        assert(tier0.try_append(forged, completion) ==
               recorder::AppendStatus::Committed);
        assert(tier0.read(1U, output_batch, output_token) ==
               recorder::ReadStatus::Ok);
    };
    recorder::DurableBatch legacy_batch;
    recorder::DurableToken legacy_token;
    make_compatibility_batch(
        unapplied_batch,
        {legacy_payload.data(), legacy_size},
        1U,
        legacy_batch,
        legacy_token);
    RouteKernel legacy_kernel = make_reference_kernel(
        unapplied_log.configuration, *unapplied_log.candidates);
    RecoveryLedger legacy_ledger(
        legacy_kernel,
        recovery_configuration(*unapplied_log.candidates));
    {
        allocation_probe::Scope no_allocations;
        assert(legacy_ledger.replay_dispatch_batch(
            legacy_token, legacy_batch).status ==
            DispatchReplayStatus::Applied);
        assert(no_allocations.allocations() == 0U);
    }

    recorder::DurableBatch adaptive_v2_batch;
    recorder::DurableToken adaptive_v2_token;
    make_compatibility_batch(
        eligible_batch,
        canonical,
        2U,
        adaptive_v2_batch,
        adaptive_v2_token);
    RouteKernel adaptive_v2_kernel = make_reference_kernel(
        eligible_log.configuration, *eligible_log.candidates);
    RecoveryLedger adaptive_v2_ledger(
        adaptive_v2_kernel,
        recovery_configuration(*eligible_log.candidates));
    assert(adaptive_v2_ledger.replay_dispatch_batch(
               adaptive_v2_token, adaptive_v2_batch).status ==
           DispatchReplayStatus::Applied);
}

void test_checkpoint_restore_then_later_replay() {
    CanonicalLog log;
    assert(log.dispatch(1U, 1U).dispatched());
    assert(log.dispatch(2U, 2U).dispatched());
    recorder::DurableBatch first_batch;
    recorder::DurableToken first_token;
    recorder::DurableBatch second_batch;
    recorder::DurableToken second_token;
    assert(log.recorder.read(1U, first_batch, first_token) ==
           recorder::ReadStatus::Ok);
    assert(log.recorder.read(2U, second_batch, second_token) ==
           recorder::ReadStatus::Ok);

    RouteKernel first_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger first(first_kernel, recovery_configuration(*log.candidates));
    assert(first.replay_dispatch_batch(first_token, first_batch).status ==
           DispatchReplayStatus::Applied);
    std::vector<std::byte> checkpoint(first.checkpoint_size_required());
    {
        allocation_probe::Scope no_allocations;
        const CheckpointEncodeResult encoded =
            first.encode_checkpoint(checkpoint);
        assert(encoded.status == CheckpointEncodeStatus::Encoded);
        assert(encoded.encoded_size == checkpoint.size());
        assert(no_allocations.allocations() == 0U);
    }

    RouteKernel restored_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger restored(
        restored_kernel, recovery_configuration(*log.candidates));
    {
        allocation_probe::Scope no_allocations;
        const CheckpointRestoreResult result =
            restored.restore_checkpoint(checkpoint);
        assert(result.status == CheckpointRestoreStatus::Restored);
        assert(result.replay_through_batch_id == 1U);
        assert(no_allocations.allocations() == 0U);
    }
    assert(restored_kernel.active_parent_count() == 1U);
    assert(restored.replay_dispatch_batch(second_token, second_batch).status ==
           DispatchReplayStatus::Applied);
    assert(restored_kernel.active_parent_count() == 2U);
    assert(restored_kernel.recovery_identifiers().next_child_order_id == 5U);

    std::vector<std::byte> corrupt = checkpoint;
    corrupt[20U] ^= std::byte{1U};
    RouteKernel corrupt_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger corrupt_ledger(
        corrupt_kernel, recovery_configuration(*log.candidates));
    assert(corrupt_ledger.restore_checkpoint(corrupt).status ==
           CheckpointRestoreStatus::IntegrityFailure);
    assert(corrupt_kernel.active_parent_count() == 0U);

    RouteKernel mismatch_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger mismatch(
        mismatch_kernel,
        recovery_configuration(
            *log.candidates, kConfigurationGeneration + 1U));
    assert(mismatch.restore_checkpoint(checkpoint).status ==
           CheckpointRestoreStatus::ConfigurationMismatch);
}

void test_checkpoint_retains_consumed_credits_after_terminal() {
    CanonicalLog log;
    assert(log.dispatch(1U, 1U).dispatched());
    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(log.recorder.read(1U, batch, token) == recorder::ReadStatus::Ok);
    RouteKernel kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger ledger(kernel, recovery_configuration(*log.candidates));
    assert(ledger.replay_dispatch_batch(token, batch).status ==
           DispatchReplayStatus::Applied);
    assert(ledger.apply_record(record(
        RecoveryRecordType::Terminal,
        1U,
        1U,
        11U,
        1U,
        1U,
        1U)).status == RecoveryRecordStatus::Applied);
    assert(ledger.apply_record(record(
        RecoveryRecordType::Terminal,
        2U,
        2U,
        12U,
        1U,
        2U,
        2U)).status == RecoveryRecordStatus::Applied);
    assert(kernel.active_parent_count() == 0U);
    assert(kernel.venue_counters(kVenueOne).reserved_normal_tx_credits == 1U);
    assert(kernel.venue_counters(kVenueTwo).reserved_throttle_credits == 1U);

    std::vector<std::byte> checkpoint(ledger.checkpoint_size_required());
    assert(ledger.encode_checkpoint(checkpoint).status ==
           CheckpointEncodeStatus::Encoded);
    RouteKernel restored_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger restored(
        restored_kernel, recovery_configuration(*log.candidates));
    assert(restored.restore_checkpoint(checkpoint).status ==
           CheckpointRestoreStatus::Restored);
    assert(restored_kernel.active_parent_count() == 0U);
    assert(restored_kernel.venue_counters(kVenueOne)
               .reserved_normal_tx_credits == 1U);
    assert(restored_kernel.venue_counters(kVenueTwo)
               .reserved_throttle_credits == 1U);
    assert(restored.complete_reconciliation());
}

void test_fixed_capacity_fail_closed() {
    CanonicalLog log;
    assert(log.dispatch(1U, 1U).dispatched());
    assert(log.dispatch(2U, 2U).dispatched());
    recorder::DurableBatch first_batch;
    recorder::DurableToken first_token;
    recorder::DurableBatch second_batch;
    recorder::DurableToken second_token;
    assert(log.recorder.read(1U, first_batch, first_token) ==
           recorder::ReadStatus::Ok);
    assert(log.recorder.read(2U, second_batch, second_token) ==
           recorder::ReadStatus::Ok);

    RecoveryConfiguration parent_capacity =
        recovery_configuration(*log.candidates);
    parent_capacity.active_parent_capacity = 1U;
    RouteKernel parent_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger parent_ledger(parent_kernel, parent_capacity);
    assert(parent_ledger.replay_dispatch_batch(first_token, first_batch).status ==
           DispatchReplayStatus::Applied);
    assert(parent_ledger.replay_dispatch_batch(second_token, second_batch).status ==
           DispatchReplayStatus::CapacityExceeded);
    assert(parent_kernel.active_parent_count() == 1U);

    RecoveryConfiguration dedupe_capacity =
        recovery_configuration(*log.candidates);
    dedupe_capacity.client_dedupe_capacity = 1U;
    RouteKernel dedupe_kernel =
        make_reference_kernel(log.configuration, *log.candidates);
    RecoveryLedger dedupe_ledger(dedupe_kernel, dedupe_capacity);
    assert(dedupe_ledger.replay_dispatch_batch(first_token, first_batch).status ==
           DispatchReplayStatus::Applied);
    assert(dedupe_ledger.apply_record(record(
        RecoveryRecordType::ReconcileWorking,
        1U,
        1U,
        21U,
        1U,
        1U,
        1U)).status == RecoveryRecordStatus::Applied);
    assert(dedupe_ledger.apply_record(record(
        RecoveryRecordType::CancelIntent,
        2U,
        2U,
        22U,
        1U,
        2U,
        2U)).status == RecoveryRecordStatus::CapacityExceeded);
    assert(dedupe_ledger.client_sequence_high_water(99U) == 1U);
}

void test_recorder_torn_and_corrupt_rejection() {
    recorder::InMemoryMirroredTier0 tier0({8U, 1U, 1U, false});
    recorder::DurableBatch batch(recorder::DurabilityLane::Normal, 1U);
    batch.set_leadership_epoch(kEpoch);
    batch.set_configuration_generation(kConfigurationGeneration);
    assert(batch.set_authorization_envelope_version(
        recorder::AuthorizationEnvelopeVersion::LegacyRangeV0));
    assert(batch.set_dispatch_authorization({1U, 1U, 1U}));
    const std::array payload{std::byte{0x01U}};
    assert(batch.try_append_record(1U, 1U, payload));
    recorder::CompletionSlot completion;
    tier0.set_next_append_fault_for_testing(
        recorder::ReferenceFaultPoint::AfterPrimaryWrite);
    assert(tier0.try_append(batch, completion) ==
           recorder::AppendStatus::TornCommitRejected);
    recorder::DurableBatch output;
    recorder::DurableToken token;
    assert(tier0.read(1U, output, token) != recorder::ReadStatus::Ok);

    recorder::CompletionSlot committed;
    assert(tier0.try_append(batch, committed) ==
           recorder::AppendStatus::Committed);
    assert(tier0.corrupt_payload_for_testing(
        1U, recorder::MirrorCopy::Primary, 0U));
    assert(tier0.read(1U, output, token) == recorder::ReadStatus::Corrupt);
}

}  // namespace

int main() {
    test_durable_before_kernel_commit();
    test_after_commit_before_complete_enqueue_boundary();
    test_commit_boundary_idempotency_integrity_and_allocations();
    test_partial_tx_ambiguity_and_terminal_retirement();
    test_canonical_record_cross_validation();
    test_dispatch_v3_adaptive_schema_and_legacy_v1();
    test_checkpoint_restore_then_later_replay();
    test_checkpoint_retains_consumed_credits_after_terminal();
    test_fixed_capacity_fail_closed();
    test_recorder_torn_and_corrupt_rejection();
    return 0;
}
