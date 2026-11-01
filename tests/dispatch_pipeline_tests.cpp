#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/fastpath/dispatch_pipeline.hpp"

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

namespace {

std::atomic<std::uint64_t> allocation_calls{};

}  // namespace

void* operator new(const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void operator delete(void* const value) noexcept {
    std::free(value);
}

void operator delete[](void* const value) noexcept {
    std::free(value);
}

void operator delete(void* const value, const std::size_t) noexcept {
    std::free(value);
}

void operator delete[](void* const value, const std::size_t) noexcept {
    std::free(value);
}

namespace {

using namespace sorx;
using namespace sorx::dataplane;
using namespace sorx::fastpath;

static_assert(sizeof(recorder::ExternalEffectAuthorization) == 88U);
static_assert(sizeof(recorder::ExternalEffectAuthorizationList) == 2'824U);
static_assert(sizeof(recorder::DurableEffectPermit) == 160U);
static_assert(sizeof(recorder::DurableToken) == 3'000U);
static_assert(sizeof(EncodedFrame) == 584U);
static_assert(sizeof(AuthorizedFrame) == 744U);
static_assert(sizeof(AuthorizedFrame) < sizeof(recorder::DurableToken));
static_assert(
    sizeof(AuthorizedFrame) <= sizeof(EncodedFrame) +
                                   sizeof(recorder::DurableEffectPermit));

constexpr VenueId kVenueOne{1U};
constexpr VenueId kVenueTwo{2U};
constexpr InstrumentId kInstrument{11U};
constexpr std::uint64_t kLeadershipEpoch = 7U;
constexpr std::uint64_t kConfigurationGeneration = 13U;

[[nodiscard]] RouteConfig route(
    const VenueId venue,
    const std::uint32_t priority) {
    RouteConfig output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.venue_name = venue == kVenueOne ? "VENUE_ONE" : "VENUE_TWO";
    output.canonical_symbol = "TEST";
    output.wire_symbol = venue == kVenueOne ? "TEST1" : "TEST2";
    output.capabilities = VenueCapabilities{true, true};
    output.allocation_cap = Quantity{100U};
    output.aggressive_priority = priority;
    output.passive_priority = priority;
    output.configured_latency_ns = 100U + priority;
    return output;
}

[[nodiscard]] std::shared_ptr<const CandidateTable> candidate_table() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(
        std::vector<RouteConfig>{route(kVenueOne, 1U), route(kVenueTwo, 2U)},
        errors));
    assert(errors.empty());
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
    output.risk.venues.push_back(VenueRiskLimit{
        kVenueOne, true, Quantity{10'000U}});
    output.risk.venues.push_back(VenueRiskLimit{
        kVenueTwo, true, Quantity{10'000U}});
    return output;
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
    assert(book.push_bid(md::Level{
        Price{ask.units - 2}, Quantity{10U}, 1U}));
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
    output.normal_tx_credits = 1U;
    output.cancel_reserved_tx_credits = 2U;
    output.throttle_credits = 4U;
    output.has_book = true;
    output.source_healthy = true;
    output.conversion_compatible = true;
    output.risk_allowed = true;
    assert(output.valid());
    return output;
}

[[nodiscard]] std::array<BookSlotView, 2U> live_views() {
    return {
        live_view(kVenueOne, Price{100}, Quantity{4U}),
        live_view(kVenueTwo, Price{101}, Quantity{6U}),
    };
}

[[nodiscard]] ParentOrder parent(
    const std::uint64_t id = 1U,
    const Price limit = Price{101}) noexcept {
    return ParentOrder{
        ParentOrderId{id},
        kInstrument,
        Side::Buy,
        Quantity{10U},
        limit,
        RoutingIntent::AggressiveIOC,
    };
}

[[nodiscard]] DispatchRequestMetadata metadata(
    const std::uint64_t sequence = 91U) noexcept {
    return DispatchRequestMetadata{sequence, 1'050U, 1'060U, 1'055U};
}

void put_u32(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    destination[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    destination[offset + 1U] =
        static_cast<std::byte>((value >> 16U) & 0xffU);
    destination[offset + 2U] =
        static_cast<std::byte>((value >> 8U) & 0xffU);
    destination[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

void put_u64(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index{}; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        destination[offset + index] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

class DeterministicEncoder final : public IChildFrameEncoder {
public:
    [[nodiscard]] FrameEncodeResult encode_new(
        const ParentOrder& order,
        const ChildAllocation& allocation,
        const PlannedChild& child,
        const std::span<std::byte> destination) noexcept override {
        constexpr std::size_t kFixedBytes = 46U;
        if (fail) {
            return {FrameEncodeStatus::Failed, 0U};
        }
        if (!order.id || !allocation.venue || !child.child_order_id ||
            child.wire_identity.size > child.wire_identity.bytes.size()) {
            return {FrameEncodeStatus::InvalidInput, 0U};
        }
        const std::size_t required = kFixedBytes + child.wire_identity.size;
        if (required > destination.size()) {
            return {FrameEncodeStatus::BufferTooSmall, 0U};
        }
        destination[0] = std::byte{0x4eU};
        put_u32(destination, 1U, allocation.venue.value());
        put_u64(destination, 5U, order.id.value());
        put_u64(destination, 13U, child.child_order_id.value());
        put_u64(destination, 21U, child.dispatch_attempt_id);
        put_u64(destination, 29U, allocation.venue_quantity.units);
        put_u64(
            destination,
            37U,
            static_cast<std::uint64_t>(allocation.venue_limit_price.units));
        destination[45U] = static_cast<std::byte>(child.wire_identity.size);
        std::copy_n(
            child.wire_identity.bytes.begin(),
            child.wire_identity.size,
            destination.begin() + kFixedBytes);
        return {
            FrameEncodeStatus::Encoded,
            static_cast<std::uint16_t>(required),
        };
    }

    bool fail{};
};

class CountingRecorder final : public recorder::IFlightRecorder {
public:
    explicit CountingRecorder(recorder::IFlightRecorder& inner) noexcept
        : inner_(&inner) {}

    [[nodiscard]] recorder::AppendStatus try_append(
        const recorder::DurableBatch& batch,
        recorder::CompletionSlot& completion) noexcept override {
        ++append_calls;
        return inner_->try_append(batch, completion);
    }

    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableToken& token,
        const recorder::DurabilityDomain required) const noexcept override {
        return inner_->authorizes_external_effect(token, required);
    }
    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableEffectPermit& permit,
        const recorder::DurabilityDomain required) const noexcept override {
        return inner_->authorizes_external_effect(permit, required);
    }

    [[nodiscard]] recorder::ReadStatus read(
        const std::uint64_t batch_id,
        recorder::DurableBatch& batch,
        recorder::DurableToken& token) const noexcept override {
        return inner_->read(batch_id, batch, token);
    }

    [[nodiscard]] recorder::ReplayStatus replay(
        const recorder::ReplayRange range,
        recorder::IReplaySink& sink) const noexcept override {
        return inner_->replay(range, sink);
    }

    [[nodiscard]] recorder::AcknowledgeStatus acknowledge(
        const std::uint64_t through) noexcept override {
        return inner_->acknowledge(through);
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

    std::uint64_t append_calls{};

private:
    recorder::IFlightRecorder* inner_{};
};

class MissingTokenRecorder final : public recorder::IFlightRecorder {
public:
    [[nodiscard]] recorder::AppendStatus try_append(
        const recorder::DurableBatch&,
        recorder::CompletionSlot&) noexcept override {
        ++append_calls;
        return recorder::AppendStatus::Committed;
    }

    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableToken&,
        recorder::DurabilityDomain) const noexcept override {
        return false;
    }
    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableEffectPermit&,
        recorder::DurabilityDomain) const noexcept override {
        return false;
    }

    [[nodiscard]] recorder::ReadStatus read(
        std::uint64_t,
        recorder::DurableBatch&,
        recorder::DurableToken&) const noexcept override {
        return recorder::ReadStatus::InvalidBatchId;
    }

    [[nodiscard]] recorder::ReplayStatus replay(
        recorder::ReplayRange,
        recorder::IReplaySink&) const noexcept override {
        return recorder::ReplayStatus::Empty;
    }

    [[nodiscard]] recorder::AcknowledgeStatus acknowledge(
        std::uint64_t) noexcept override {
        return recorder::AcknowledgeStatus::InvalidBatchId;
    }

    [[nodiscard]] recorder::DrainStatus drain_to_tier1(
        recorder::ReplayRange,
        recorder::ITier1SegmentSink&) noexcept override {
        return recorder::DrainStatus::Empty;
    }

    [[nodiscard]] recorder::FlightRecorderMetadata metadata()
        const noexcept override {
        return {};
    }

    [[nodiscard]] recorder::DurabilityDomain durability_domain()
        const noexcept override {
        return recorder::DurabilityDomain::VolatileSoftwareReference;
    }

    std::uint64_t append_calls{};
};

class CountingTransport final : public IFrameTransport {
public:
    [[nodiscard]] TransportWriteResult try_transmit(
        const std::span<const std::byte> frame) noexcept override {
        ++calls;
        last_size = frame.size();
        if (!frame.empty()) {
            first_byte = frame[0];
        }
        return result;
    }

    TransportWriteResult result{TransportWriteResult::Sent};
    std::uint64_t calls{};
    std::size_t last_size{};
    std::byte first_byte{};
};

struct Fixture final {
    Fixture()
        : candidates(candidate_table()),
          configuration(make_configuration()),
          kernel(
              configuration,
              *candidates,
              AdaptivePolicyMode::Optional,
              WireIdentityMode::ReferenceGeneric),
          tier0({32U, 4U, 4U, false}),
          recorder(tier0),
          pipeline(
              kernel,
              recorder,
              registry,
              DispatchPipelinePolicy{
                  kLeadershipEpoch,
                  kConfigurationGeneration,
                  recorder::DurabilityDomain::VolatileSoftwareReference}) {
        assert(registry.register_venue(kVenueOne, venue_one, encoder_one) ==
               VenueRegistrationStatus::Registered);
        assert(registry.register_venue(kVenueTwo, venue_two, encoder_two) ==
               VenueRegistrationStatus::Registered);
        assert(registry.seal());
    }

    std::shared_ptr<const CandidateTable> candidates;
    CoreConfiguration configuration;
    RouteKernel kernel;
    recorder::InMemoryMirroredTier0 tier0;
    CountingRecorder recorder;
    VenueTxQueues venue_one;
    VenueTxQueues venue_two;
    DeterministicEncoder encoder_one;
    DeterministicEncoder encoder_two;
    FixedVenueRegistry registry;
    DispatchPipeline pipeline;
};

[[nodiscard]] recorder::DurableToken make_token(
    recorder::InMemoryMirroredTier0& recorder,
    const std::span<const EncodedFrame> frames,
    const std::uint64_t producer_sequence = 1U) {
    assert(!frames.empty());
    recorder::DurableBatch batch(
        recorder::DurabilityLane::Cancel, producer_sequence);
    batch.set_leadership_epoch(kLeadershipEpoch);
    batch.set_configuration_generation(kConfigurationGeneration);
    assert(batch.set_dispatch_authorization(
        {frames.front().child_id,
         frames.front().dispatch_attempt_id,
         static_cast<std::uint32_t>(frames.size())}));
    for (const EncodedFrame& encoded : frames) {
        assert(batch.try_add_external_effect_authorization(
            encoded.venue_id,
            encoded.child_id,
            encoded.dispatch_attempt_id,
            encoded.venue_session_id,
            encoded.venue_session_epoch,
            encoded.ownership_generation,
            static_cast<std::uint8_t>(encoded.dispatch_class),
            encoded.bytes()));
    }
    const std::array payload{std::byte{0x55U}};
    assert(batch.try_append_record(1U, 1U, payload));
    recorder::CompletionSlot completion;
    assert(recorder.try_append(batch, completion) ==
           recorder::AppendStatus::Committed);
    const auto token = completion.token();
    assert(token.has_value());
    return *token;
}

[[nodiscard]] EncodedFrame frame(
    const std::uint32_t venue,
    const std::uint64_t child,
    const std::uint64_t attempt,
    const DispatchClass dispatch_class,
    const std::byte marker) noexcept {
    EncodedFrame output;
    output.venue_id = venue;
    output.child_id = child;
    output.dispatch_attempt_id = attempt;
    output.leadership_epoch = kLeadershipEpoch;
    output.configuration_generation = kConfigurationGeneration;
    output.venue_session_id = 1U;
    output.venue_session_epoch = 1U;
    output.ownership_generation = 1U;
    output.dispatch_class = dispatch_class;
    output.size = 1U;
    output.storage[0] = marker;
    assert(output.valid());
    return output;
}

void assert_same_frame(
    const EncodedFrame& lhs,
    const EncodedFrame& rhs) {
    assert(lhs.venue_id == rhs.venue_id);
    assert(lhs.child_id == rhs.child_id);
    assert(lhs.dispatch_attempt_id == rhs.dispatch_attempt_id);
    assert(lhs.leadership_epoch == rhs.leadership_epoch);
    assert(lhs.configuration_generation == rhs.configuration_generation);
    assert(lhs.venue_session_id == rhs.venue_session_id);
    assert(lhs.venue_session_epoch == rhs.venue_session_epoch);
    assert(lhs.ownership_generation == rhs.ownership_generation);
    assert(lhs.dispatch_class == rhs.dispatch_class);
    assert(lhs.size == rhs.size);
    assert(std::equal(lhs.bytes().begin(), lhs.bytes().end(), rhs.bytes().begin()));
}

void test_success_one_append_and_deterministic_records() {
    Fixture first;
    const auto views = live_views();
    const std::uint64_t before = allocation_calls.load(std::memory_order_relaxed);
    const DispatchPipelineResult result =
        first.pipeline.try_dispatch(parent(), views, metadata());
    const std::uint64_t after = allocation_calls.load(std::memory_order_relaxed);
    assert(after == before);
    assert(result.dispatched());
    assert(result.child_count == 2U);
    assert(first.recorder.append_calls == 1U);
    assert(first.kernel.active_parent_count() == 1U);
    assert(first.kernel.instrument_counters(kInstrument).gross_outstanding ==
           Quantity{10U});
    assert(first.venue_one.normal_size_approx() == 1U);
    assert(first.venue_two.normal_size_approx() == 1U);

    recorder::DurableBatch first_batch;
    recorder::DurableToken first_token;
    assert(first.tier0.read(
               result.token.batch_id, first_batch, first_token) ==
           recorder::ReadStatus::Ok);
    assert(first_batch.record_count() == 4U);
    assert(first_batch.record(0U)->type ==
           static_cast<std::uint16_t>(DispatchRecordType::ParentCommand));
    assert(first_batch.record(1U)->type ==
           static_cast<std::uint16_t>(DispatchRecordType::PinnedRoutingInputs));
    assert(first_batch.record(2U)->type == static_cast<std::uint16_t>(
           DispatchRecordType::RouteAndRiskDecision));
    assert(first_batch.record(3U)->type ==
           static_cast<std::uint16_t>(DispatchRecordType::ChildDispatches));
    const recorder::DispatchAuthorization expected_authorization{
        1U, 1U, 2U};
    assert(first_token.dispatch_authorization == expected_authorization);

    AuthorizedFrame first_one;
    AuthorizedFrame first_two;
    assert(first.venue_one.try_pop_normal(first_one));
    assert(first.venue_two.try_pop_normal(first_two));
    assert(first_one.frame.storage[0] == std::byte{0x4eU});
    assert(first_two.frame.storage[0] == std::byte{0x4eU});
    assert(first_one.frame.child_id == 1U);
    assert(first_two.frame.child_id == 2U);

    Fixture repeated;
    const DispatchPipelineResult repeated_result =
        repeated.pipeline.try_dispatch(parent(), views, metadata());
    assert(repeated_result.dispatched());
    assert(repeated.recorder.append_calls == 1U);
    recorder::DurableBatch repeated_batch;
    recorder::DurableToken repeated_token;
    assert(repeated.tier0.read(
               repeated_result.token.batch_id,
               repeated_batch,
               repeated_token) == recorder::ReadStatus::Ok);
    assert(first_batch.encoded_size() == repeated_batch.encoded_size());
    assert(std::equal(
        first_batch.encoded_bytes().begin(),
        first_batch.encoded_bytes().end(),
        repeated_batch.encoded_bytes().begin()));
    assert(first_token == repeated_token);
    AuthorizedFrame repeated_one;
    AuthorizedFrame repeated_two;
    assert(repeated.venue_one.try_pop_normal(repeated_one));
    assert(repeated.venue_two.try_pop_normal(repeated_two));
    assert_same_frame(first_one.frame, repeated_one.frame);
    assert_same_frame(first_two.frame, repeated_two.frame);
}

void test_no_append_on_plan_or_queue_failure() {
    {
        Fixture fixture;
        const auto views = live_views();
        const DispatchPipelineResult rejected = fixture.pipeline.try_dispatch(
            parent(1U, Price{99}), views, metadata());
        assert(rejected.status == DispatchPipelineStatus::PlanRejected);
        assert(fixture.recorder.append_calls == 0U);
        assert(fixture.kernel.active_parent_count() == 0U);
    }
    {
        Fixture fixture;
        const EncodedFrame queued = frame(
            kVenueOne.value(),
            1'000U,
            1'000U,
            DispatchClass::NewOrReplace,
            std::byte{0x11U});
        const std::array queued_frames{queued};
        const recorder::DurableToken token =
            make_token(fixture.tier0, queued_frames);
        const auto permit = token.effect_permit(0U);
        assert(permit.has_value());
        for (std::uint64_t offset{};
             offset < kNormalVenueQueueCapacity;
             ++offset) {
            static_cast<void>(offset);
            assert(fixture.venue_one.try_enqueue_normal(
                queued,
                *permit));
        }
        const auto views = live_views();
        const DispatchPipelineResult saturated = fixture.pipeline.try_dispatch(
            parent(), views, metadata());
        assert(saturated.status ==
               DispatchPipelineStatus::NormalQueueCapacityUnavailable);
        assert(fixture.recorder.append_calls == 0U);
        assert(fixture.kernel.active_parent_count() == 0U);
        assert(fixture.venue_two.normal_size_approx() == 0U);
    }
}

void test_no_append_when_native_encoding_fails() {
    Fixture fixture;
    fixture.encoder_one.fail = true;
    const auto views = live_views();
    const DispatchPipelineResult result =
        fixture.pipeline.try_dispatch(parent(), views, metadata());
    assert(result.status == DispatchPipelineStatus::FrameEncodingFailed);
    assert(result.encode_status == FrameEncodeStatus::Failed);
    assert(fixture.recorder.append_calls == 0U);
    assert(fixture.kernel.active_parent_count() == 0U);
    assert(fixture.venue_one.normal_size_approx() == 0U);
    assert(fixture.venue_two.normal_size_approx() == 0U);
}

void test_missing_token_never_commits_or_enqueues() {
    const auto candidates = candidate_table();
    const CoreConfiguration configuration = make_configuration();
    RouteKernel kernel(
        configuration,
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric);
    MissingTokenRecorder recorder;
    VenueTxQueues one;
    VenueTxQueues two;
    DeterministicEncoder encoder_one;
    DeterministicEncoder encoder_two;
    FixedVenueRegistry registry;
    assert(registry.register_venue(kVenueOne, one, encoder_one) ==
           VenueRegistrationStatus::Registered);
    assert(registry.register_venue(kVenueTwo, two, encoder_two) ==
           VenueRegistrationStatus::Registered);
    assert(registry.seal());
    DispatchPipeline pipeline(
        kernel,
        recorder,
        registry,
        {kLeadershipEpoch,
         kConfigurationGeneration,
         recorder::DurabilityDomain::VolatileSoftwareReference});
    const auto views = live_views();
    const DispatchPipelineResult result =
        pipeline.try_dispatch(parent(), views, metadata());
    assert(result.status == DispatchPipelineStatus::DurableTokenUnavailable);
    assert(result.recovery.required);
    assert(result.recovery.durable_batch_id == 0U);
    assert(result.recovery.producer_sequence == metadata().producer_sequence);
    assert(result.recovery.applied_phase ==
           DispatchRecoveryPhase::DurableOnly);
    assert(pipeline.fail_stopped());
    assert(recorder.append_calls == 1U);
    assert(kernel.active_parent_count() == 0U);
    assert(one.normal_size_approx() == 0U);
    assert(two.normal_size_approx() == 0U);
    const DispatchPipelineResult stopped =
        pipeline.try_dispatch(parent(), views, metadata());
    assert(stopped.status == DispatchPipelineStatus::FailStopped);
    assert(stopped.recovery == pipeline.recovery_requirement());
    assert(recorder.append_calls == 1U);
}

void test_successful_dispatch_reaches_both_token_gated_sessions() {
    Fixture fixture;
    const auto views = live_views();
    const DispatchPipelineResult result =
        fixture.pipeline.try_dispatch(parent(), views, metadata());
    assert(result.dispatched());

    CountingTransport transport_one;
    CountingTransport transport_two;
    TokenGatedOrderEntrySession session_one(
        {kVenueOne.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         1U,
         1U},
        fixture.recorder,
        transport_one);
    TokenGatedOrderEntrySession session_two(
        {kVenueTwo.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         1U,
         1U},
        fixture.recorder,
        transport_two);
    TxOwner owner_one(fixture.venue_one, session_one);
    TxOwner owner_two(fixture.venue_two, session_two);
    assert(owner_one.poll_once() == TxPollResult::SentNormal);
    assert(owner_two.poll_once() == TxPollResult::SentNormal);
    assert(transport_one.calls == 1U);
    assert(transport_two.calls == 1U);
    assert(transport_one.first_byte == std::byte{0x4eU});
    assert(transport_two.first_byte == std::byte{0x4eU});
    assert(owner_one.statistics().sent_normal == 1U);
    assert(owner_two.statistics().sent_normal == 1U);
}

void assert_recovery_locator(
    const TxRecoveryRequirement& recovery,
    const EncodedFrame& failed,
    const recorder::DurableEffectPermit& permit,
    const SendResult rejection) {
    assert(recovery.required);
    assert(recovery.durable_batch_id == permit.batch_id);
    assert(recovery.producer_sequence == permit.producer_sequence);
    assert(recovery.leadership_epoch == permit.leadership_epoch);
    assert(recovery.configuration_generation ==
           permit.configuration_generation);
    assert(recovery.venue_id == failed.venue_id);
    assert(recovery.child_id == failed.child_id);
    assert(recovery.dispatch_attempt_id == failed.dispatch_attempt_id);
    assert(recovery.venue_session_id == failed.venue_session_id);
    assert(recovery.venue_session_epoch == failed.venue_session_epoch);
    assert(recovery.ownership_generation == failed.ownership_generation);
    assert(recovery.effect_ordinal == permit.effect_ordinal);
    assert(recovery.lane == permit.lane);
    assert(recovery.domain == permit.domain);
    assert(recovery.rejection == rejection);
    assert(recovery.frame_hash == permit.authorization.frame_hash);
    assert(recovery.batch_hash == permit.batch_hash);
}

void test_nonambiguous_normal_rejection_preserves_cancel_lane() {
    recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, false});
    const std::array frames{
        frame(
            kVenueOne.value(), 1U, 1U, DispatchClass::NewOrReplace,
            std::byte{0x4eU}),
        frame(
            kVenueOne.value(), 2U, 2U,
            DispatchClass::CancelOrReconcile, std::byte{0xccU}),
    };
    const recorder::DurableToken token = make_token(recorder, frames);
    auto invalid_normal = *token.effect_permit(0U);
    const auto cancel_permit = token.effect_permit(1U);
    assert(cancel_permit.has_value());
    invalid_normal.batch_hash[0U] ^= std::byte{0x01U};
    VenueTxQueues queues;
    assert(queues.try_enqueue_normal(frames[0U], invalid_normal));
    CountingTransport transport;
    TokenGatedOrderEntrySession session(
        {kVenueOne.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         1U,
         1U},
        recorder,
        transport);
    TxOwner owner(queues, session);
    assert(owner.poll_once() == TxPollResult::RecoveryRequired);
    assert(owner.statistics().last_rejection ==
           SendResult::MissingOrInvalidDurableToken);
    assert(owner.recovery_required());
    const TxRecoveryRequirement recovery = owner.recovery_requirement();
    assert_recovery_locator(
        recovery, frames[0U], invalid_normal,
        SendResult::MissingOrInvalidDurableToken);
    assert(!recovery.delivery_ambiguous);
    assert(!recovery.session_fatal);
    assert(!recovery.cancel_or_reconcile);
    assert(transport.calls == 0U);

    assert(queues.try_enqueue_cancel(frames[1U], *cancel_permit));
    assert(owner.poll_once() == TxPollResult::SentCancel);
    assert(transport.calls == 1U);
    assert(owner.poll_once() == TxPollResult::RecoveryRequired);
    assert(owner.recovery_requirement() == recovery);
}

void test_ambiguous_transport_failure_blocks_all_lanes() {
    recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, false});
    const std::array frames{
        frame(
            kVenueOne.value(), 10U, 10U, DispatchClass::NewOrReplace,
            std::byte{0x4eU}),
        frame(
            kVenueOne.value(), 11U, 11U,
            DispatchClass::CancelOrReconcile, std::byte{0xccU}),
    };
    const recorder::DurableToken token = make_token(recorder, frames, 2U);
    const auto normal_permit = token.effect_permit(0U);
    const auto cancel_permit = token.effect_permit(1U);
    assert(normal_permit.has_value() && cancel_permit.has_value());
    VenueTxQueues queues;
    assert(queues.try_enqueue_normal(frames[0U], *normal_permit));
    CountingTransport transport;
    transport.result = TransportWriteResult::Failed;
    TokenGatedOrderEntrySession session(
        {kVenueOne.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         1U,
         1U},
        recorder,
        transport);
    TxOwner owner(queues, session);
    assert(owner.poll_once() == TxPollResult::RecoveryRequired);
    const TxRecoveryRequirement recovery = owner.recovery_requirement();
    assert_recovery_locator(
        recovery, frames[0U], *normal_permit,
        SendResult::TransportFailure);
    assert(recovery.delivery_ambiguous);
    assert(!recovery.session_fatal);
    assert(!recovery.cancel_or_reconcile);
    assert(transport.calls == 1U);

    transport.result = TransportWriteResult::Sent;
    assert(queues.try_enqueue_cancel(frames[1U], *cancel_permit));
    assert(owner.poll_once() == TxPollResult::RecoveryRequired);
    assert(queues.cancel_size_approx() == 1U);
    assert(transport.calls == 1U);
}

void test_cancel_or_session_failure_blocks_all_lanes() {
    recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, false});
    const std::array cancel_frames{
        frame(
            kVenueOne.value(), 20U, 20U,
            DispatchClass::CancelOrReconcile, std::byte{0xc1U}),
        frame(
            kVenueOne.value(), 21U, 21U,
            DispatchClass::CancelOrReconcile, std::byte{0xc2U}),
    };
    const recorder::DurableToken cancel_token =
        make_token(recorder, cancel_frames, 3U);
    auto invalid_cancel = *cancel_token.effect_permit(0U);
    const auto next_cancel = cancel_token.effect_permit(1U);
    assert(next_cancel.has_value());
    invalid_cancel.batch_hash[0U] ^= std::byte{0x01U};
    VenueTxQueues cancel_queues;
    assert(cancel_queues.try_enqueue_cancel(
        cancel_frames[0U], invalid_cancel));
    assert(cancel_queues.try_enqueue_cancel(
        cancel_frames[1U], *next_cancel));
    CountingTransport cancel_transport;
    TokenGatedOrderEntrySession cancel_session(
        {kVenueOne.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         1U,
         1U},
        recorder,
        cancel_transport);
    TxOwner cancel_owner(cancel_queues, cancel_session);
    assert(cancel_owner.poll_once() == TxPollResult::RecoveryRequired);
    const TxRecoveryRequirement cancel_recovery =
        cancel_owner.recovery_requirement();
    assert(cancel_recovery.cancel_or_reconcile);
    assert(!cancel_recovery.delivery_ambiguous);
    assert(cancel_owner.poll_once() == TxPollResult::RecoveryRequired);
    assert(cancel_queues.cancel_size_approx() == 1U);
    assert(cancel_transport.calls == 0U);

    const std::array session_frames{
        frame(
            kVenueOne.value(), 30U, 30U, DispatchClass::NewOrReplace,
            std::byte{0x4eU}),
        frame(
            kVenueOne.value(), 31U, 31U,
            DispatchClass::CancelOrReconcile, std::byte{0xccU}),
    };
    const recorder::DurableToken session_token =
        make_token(recorder, session_frames, 4U);
    const auto session_normal = session_token.effect_permit(0U);
    const auto session_cancel = session_token.effect_permit(1U);
    assert(session_normal.has_value() && session_cancel.has_value());
    VenueTxQueues session_queues;
    assert(session_queues.try_enqueue_normal(
        session_frames[0U], *session_normal));
    CountingTransport session_transport;
    TokenGatedOrderEntrySession wrong_session(
        {kVenueOne.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         2U,
         1U},
        recorder,
        session_transport);
    TxOwner session_owner(session_queues, wrong_session);
    assert(session_owner.poll_once() == TxPollResult::RecoveryRequired);
    const TxRecoveryRequirement session_recovery =
        session_owner.recovery_requirement();
    assert(session_recovery.session_fatal);
    assert(!session_recovery.delivery_ambiguous);
    assert(session_recovery.rejection == SendResult::WrongVenue);
    assert(session_queues.try_enqueue_cancel(
        session_frames[1U], *session_cancel));
    assert(session_owner.poll_once() == TxPollResult::RecoveryRequired);
    assert(session_queues.cancel_size_approx() == 1U);
    assert(session_transport.calls == 0U);
}

void test_cancel_priority_survives_normal_saturation() {
    recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, false});
    const std::array frames{
        frame(
            kVenueOne.value(), 1U, 1U, DispatchClass::NewOrReplace,
            std::byte{0x4eU}),
        frame(
            kVenueOne.value(), 2U, 2U,
            DispatchClass::CancelOrReconcile, std::byte{0xccU}),
    };
    const recorder::DurableToken token = make_token(recorder, frames);
    const auto normal_permit = token.effect_permit(0U);
    const auto cancel_permit = token.effect_permit(1U);
    assert(normal_permit.has_value() && cancel_permit.has_value());
    VenueTxQueues queues;
    for (std::uint64_t child = 1U; child <= 64U; ++child) {
        static_cast<void>(child);
        assert(queues.try_enqueue_normal(frames[0U], *normal_permit));
    }
    assert(queues.normal_available_approx() == 0U);
    assert(queues.try_enqueue_cancel(
        frames[1U],
        *cancel_permit));
    CountingTransport transport;
    TokenGatedOrderEntrySession session(
        {kVenueOne.value(),
         recorder::DurabilityDomain::VolatileSoftwareReference,
         1U,
         1U},
        recorder,
        transport);
    TxOwner owner(queues, session);
    assert(owner.poll_once() == TxPollResult::SentCancel);
    assert(transport.calls == 1U);
    assert(transport.first_byte == std::byte{0xccU});
    assert(owner.poll_once() == TxPollResult::SentNormal);
    assert(transport.calls == 2U);
    assert(transport.first_byte == std::byte{0x4eU});
}

void test_dual_durable_gate_blocks_commit_without_remote_token() {
    const auto candidates = candidate_table();
    const CoreConfiguration configuration = make_configuration();
    RouteKernel kernel(
        configuration,
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric);
    recorder::InMemoryMirroredTier0 tier0({16U, 2U, 2U, false});
    CountingRecorder recorder(tier0);
    VenueTxQueues one;
    VenueTxQueues two;
    DeterministicEncoder encoder_one;
    DeterministicEncoder encoder_two;
    FixedVenueRegistry registry;
    assert(registry.register_venue(kVenueOne, one, encoder_one) ==
           VenueRegistrationStatus::Registered);
    assert(registry.register_venue(kVenueTwo, two, encoder_two) ==
           VenueRegistrationStatus::Registered);
    assert(registry.seal());
    ha::ReplicationGate gate(1U, ha::ReplicationPolicy::DualDurable);
    assert(gate.install_authenticated_lease(
        {1U, kLeadershipEpoch, 1U, 10'000U}));
    DispatchPipeline pipeline(
        kernel,
        recorder,
        registry,
        {kLeadershipEpoch,
         kConfigurationGeneration,
         recorder::DurabilityDomain::VolatileSoftwareReference},
        &gate,
        nullptr);
    const auto views = live_views();
    const DispatchPipelineResult result =
        pipeline.try_dispatch(parent(), views, metadata());
    assert(result.status == DispatchPipelineStatus::DualDurabilityRejected);
    assert(result.fence_error == ha::DispatchFenceError::RemoteRecordNotDurable);
    assert(result.recovery.required);
    assert(result.recovery.durable_batch_id == result.token.batch_id);
    assert(result.recovery.applied_phase ==
           DispatchRecoveryPhase::DurableOnly);
    assert(pipeline.fail_stopped());
    assert(recorder.append_calls == 1U);
    assert(kernel.active_parent_count() == 0U);
    assert(one.normal_size_approx() == 0U);
    assert(two.normal_size_approx() == 0U);
    const DispatchPipelineResult stopped =
        pipeline.try_dispatch(parent(), views, metadata());
    assert(stopped.status == DispatchPipelineStatus::FailStopped);
    assert(stopped.recovery == pipeline.recovery_requirement());
    assert(recorder.append_calls == 1U);
}

}  // namespace

int main() {
    test_success_one_append_and_deterministic_records();
    test_no_append_on_plan_or_queue_failure();
    test_no_append_when_native_encoding_fails();
    test_missing_token_never_commits_or_enqueues();
    test_successful_dispatch_reaches_both_token_gated_sessions();
    test_nonambiguous_normal_rejection_preserves_cancel_lane();
    test_ambiguous_transport_failure_blocks_all_lanes();
    test_cancel_or_session_failure_blocks_all_lanes();
    test_cancel_priority_survives_normal_saturation();
    test_dual_durable_gate_blocks_commit_without_remote_token();
    return 0;
}
