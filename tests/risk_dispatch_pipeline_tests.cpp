#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/fastpath/risk_dispatch_pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> count{0U};

void record() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t size) {
    record();
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t size,
    const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* const value =
            _aligned_malloc(size == 0U ? 1U : size, alignment)) {
        return value;
    }
#else
    const std::size_t nonzero = size == 0U ? 1U : size;
    const std::size_t rounded =
        ((nonzero + alignment - 1U) / alignment) * alignment;
    if (void* const value = std::aligned_alloc(alignment, rounded)) {
        return value;
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

constexpr VenueId kVenueOne{1U};
constexpr VenueId kVenueTwo{2U};
constexpr InstrumentId kInstrument{11U};
constexpr risk::ClientId kClient = 1U;
constexpr risk::AccountId kAccount = 10U;
constexpr std::uint64_t kRiskGeneration = 7U;
constexpr std::uint64_t kLeadershipEpoch = 5U;
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

[[nodiscard]] CoreConfiguration make_route_configuration() {
    CoreConfiguration output;
    output.maximum_aggressive_venues = 2U;
    output.risk.generation = kRiskGeneration;

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

[[nodiscard]] risk::RiskLimits make_risk_limits() noexcept {
    return risk::RiskLimits{
        Quantity{100U},
        Notional{1'000'000U},
        Quantity{1'000U},
        Notional{10'000'000U},
        100U,
        Price{1},
        Price{1'000'000}};
}

[[nodiscard]] std::array<risk::AccountRiskConfig, 1U>
account_configuration() noexcept {
    return {risk::AccountRiskConfig{
        kAccount, risk::RiskPermission::NewAndCancel, make_risk_limits()}};
}

[[nodiscard]] std::array<risk::ClientRiskConfig, 1U>
client_configuration() noexcept {
    return {risk::ClientRiskConfig{
        kClient,
        kAccount,
        risk::RiskPermission::NewAndCancel,
        make_risk_limits(),
        {100U, 1U, 1'000U},
        {20U, 1U, 1'000U}}};
}

[[nodiscard]] risk::RiskKernelConfig make_risk_configuration() noexcept {
    return risk::RiskKernelConfig{
        kRiskGeneration,
        kConfigurationGeneration,
        {0x0123456789abcdefULL, 0xfedcba9876543210ULL},
        4U,
        4U,
        16U,
        32U,
        4U};
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
    output.normal_tx_credits = 2U;
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

[[nodiscard]] ParentOrder make_parent(
    const std::uint64_t parent_id = 1U,
    const std::uint64_t quantity = 10U,
    const Price limit = Price{101}) noexcept {
    return ParentOrder{
        ParentOrderId{parent_id},
        kInstrument,
        Side::Buy,
        Quantity{quantity},
        limit,
        RoutingIntent::AggressiveIOC};
}

[[nodiscard]] risk::PayloadDigest digest(const std::uint8_t seed) noexcept {
    risk::PayloadDigest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(
            static_cast<std::uint8_t>(seed + index));
    }
    return result;
}

[[nodiscard]] risk::RiskCommand make_command(
    const std::uint64_t request_id = 101U,
    const std::uint64_t parent_id = 1U,
    const std::uint64_t quantity = 10U,
    const Price limit = Price{101}) noexcept {
    return risk::RiskCommand{
        risk::RiskCommandKind::NewOrder,
        request_id,
        digest(static_cast<std::uint8_t>(request_id)),
        kClient,
        kAccount,
        ParentOrderId{parent_id},
        kInstrument,
        Side::Buy,
        Quantity{quantity},
        limit};
}

[[nodiscard]] DispatchRequestMetadata metadata(
    const std::uint64_t sequence = 91U,
    const std::uint64_t decision_time = 1'050U) noexcept {
    return DispatchRequestMetadata{
        sequence, decision_time, decision_time + 10U, decision_time + 5U};
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
    for (std::size_t index = 0U; index < 8U; ++index) {
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
        constexpr std::size_t kBytes = 45U;
        if (fail) {
            return {FrameEncodeStatus::Failed, 0U};
        }
        if (destination.size() < kBytes || !order.id || !allocation.venue ||
            !child.child_order_id) {
            return {FrameEncodeStatus::InvalidInput, 0U};
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
        return {FrameEncodeStatus::Encoded,
                static_cast<std::uint16_t>(kBytes)};
    }

    bool fail{};
};

[[nodiscard]] EncodedFrame synthetic_frame(
    const std::uint64_t id,
    const std::byte marker = std::byte{0xeeU}) noexcept {
    EncodedFrame frame;
    frame.venue_id = kVenueOne.value();
    frame.child_id = id;
    frame.dispatch_attempt_id = id;
    frame.leadership_epoch = kLeadershipEpoch;
    frame.configuration_generation = kConfigurationGeneration;
    frame.venue_session_id = 1U;
    frame.venue_session_epoch = 1U;
    frame.ownership_generation = 1U;
    frame.dispatch_class = DispatchClass::NewOrReplace;
    frame.size = 1U;
    frame.storage[0] = marker;
    return frame;
}

void saturate(VenueTxQueues& queues) {
    constexpr std::uint64_t kFirst = 10'000U;
    const EncodedFrame filler = synthetic_frame(kFirst);
    recorder::DurableBatch batch(recorder::DurabilityLane::Normal, 999U);
    batch.set_leadership_epoch(kLeadershipEpoch);
    batch.set_configuration_generation(kConfigurationGeneration);
    assert(batch.set_dispatch_authorization({kFirst, kFirst, 1U}));
    assert(batch.try_add_external_effect_authorization(
        filler.venue_id,
        filler.child_id,
        filler.dispatch_attempt_id,
        filler.venue_session_id,
        filler.venue_session_epoch,
        filler.ownership_generation,
        static_cast<std::uint8_t>(filler.dispatch_class),
        filler.bytes()));
    const std::array payload{std::byte{0x55U}};
    assert(batch.try_append_record(1U, 1U, payload));
    recorder::InMemoryMirroredTier0 reference({8U, 1U, 1U, false});
    recorder::CompletionSlot completion;
    assert(reference.try_append(batch, completion) ==
           recorder::AppendStatus::Committed);
    const auto token = completion.token();
    assert(token.has_value());
    const auto permit = token->effect_permit(0U);
    assert(permit.has_value());
    for (std::uint64_t offset = 0U;
         offset < kNormalVenueQueueCapacity;
         ++offset) {
        static_cast<void>(offset);
        assert(queues.try_enqueue_normal(filler, *permit));
    }
    assert(queues.normal_available_approx() == 0U);
}

enum class RecorderFault : std::uint8_t {
    None,
    MutateRisk,
    MutateRoute,
    SaturateVenueOne,
};

class FaultInjectingRecorder final : public recorder::IFlightRecorder {
public:
    FaultInjectingRecorder(
        recorder::IFlightRecorder& inner,
        risk::RiskKernel& risk_kernel,
        RouteKernel& route_kernel,
        VenueTxQueues& venue_one) noexcept
        : inner_(&inner),
          risk_kernel_(&risk_kernel),
          route_kernel_(&route_kernel),
          venue_one_(&venue_one) {}

    [[nodiscard]] recorder::AppendStatus try_append(
        const recorder::DurableBatch& batch,
        recorder::CompletionSlot& completion) noexcept override {
        ++append_calls;
        const recorder::AppendStatus status =
            inner_->try_append(batch, completion);
        if (status == recorder::AppendStatus::Committed && !fault_fired) {
            fault_fired = true;
            switch (fault) {
                case RecorderFault::None:
                    break;
                case RecorderFault::MutateRisk:
                    mutation_succeeded = risk_kernel_->set_kill_switch(true);
                    break;
                case RecorderFault::MutateRoute:
                    mutation_succeeded = route_kernel_->set_kill_switch(true);
                    break;
                case RecorderFault::SaturateVenueOne:
                    saturate(*venue_one_);
                    mutation_succeeded = true;
                    break;
            }
        }
        return status;
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

    RecorderFault fault{RecorderFault::None};
    bool fault_fired{};
    bool mutation_succeeded{};
    std::uint64_t append_calls{};

private:
    recorder::IFlightRecorder* inner_{};
    risk::RiskKernel* risk_kernel_{};
    RouteKernel* route_kernel_{};
    VenueTxQueues* venue_one_{};
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
        const recorder::DurabilityDomain) const noexcept override {
        return false;
    }
    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableEffectPermit&,
        const recorder::DurabilityDomain) const noexcept override {
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

struct Fixture final {
    explicit Fixture(
        const recorder::DurabilityDomain required_domain =
            recorder::DurabilityDomain::VolatileSoftwareReference,
        ha::ReplicationGate* const gate = nullptr,
        IDualDurableReplicator* const dual = nullptr)
        : candidates(candidate_table()),
          route_configuration(make_route_configuration()),
          route_kernel(
              route_configuration,
              *candidates,
              AdaptivePolicyMode::Optional,
              WireIdentityMode::ReferenceGeneric),
          accounts(account_configuration()),
          clients(client_configuration()),
          risk_kernel(
              make_risk_configuration(), accounts, clients, restricted),
          tier0({64U, 4U, 4U, false}),
          recorder(tier0, risk_kernel, route_kernel, venue_one),
          pipeline(
              risk_kernel,
              route_kernel,
              recorder,
              registry,
              {kLeadershipEpoch, kConfigurationGeneration, required_domain},
              gate,
              dual) {
        assert(registry.register_venue(kVenueOne, venue_one, encoder_one) ==
               VenueRegistrationStatus::Registered);
        assert(registry.register_venue(kVenueTwo, venue_two, encoder_two) ==
               VenueRegistrationStatus::Registered);
        assert(registry.seal());
    }

    std::shared_ptr<const CandidateTable> candidates;
    CoreConfiguration route_configuration;
    RouteKernel route_kernel;
    std::array<risk::AccountRiskConfig, 1U> accounts;
    std::array<risk::ClientRiskConfig, 1U> clients;
    std::array<InstrumentId, 0U> restricted{};
    risk::RiskKernel risk_kernel;
    recorder::InMemoryMirroredTier0 tier0;
    VenueTxQueues venue_one;
    VenueTxQueues venue_two;
    DeterministicEncoder encoder_one;
    DeterministicEncoder encoder_two;
    FixedVenueRegistry registry;
    FaultInjectingRecorder recorder;
    RiskDispatchPipeline pipeline;
};

class CanonicalReader final {
public:
    explicit CanonicalReader(const std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] bool read_u8(std::uint8_t& output) noexcept {
        if (remaining() < 1U) {
            return false;
        }
        output = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& output) noexcept {
        if (remaining() < 2U) {
            return false;
        }
        output = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(
                 std::to_integer<std::uint8_t>(bytes_[offset_])) << 8U) |
            std::to_integer<std::uint8_t>(bytes_[offset_ + 1U]));
        offset_ += 2U;
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& output) noexcept {
        if (remaining() < 4U) {
            return false;
        }
        output = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            output = (output << 8U) |
                std::to_integer<std::uint8_t>(bytes_[offset_ + index]);
        }
        offset_ += 4U;
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& output) noexcept {
        if (remaining() < 8U) {
            return false;
        }
        output = 0U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            output = (output << 8U) |
                std::to_integer<std::uint8_t>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return true;
    }

    [[nodiscard]] bool read_i64(std::int64_t& output) noexcept {
        std::uint64_t value{};
        if (!read_u64(value)) {
            return false;
        }
        output = static_cast<std::int64_t>(value);
        return true;
    }

    [[nodiscard]] bool skip(const std::size_t count) noexcept {
        if (remaining() < count) {
            return false;
        }
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool read_bytes(
        const std::size_t count,
        std::span<const std::byte>& output) noexcept {
        if (remaining() < count) {
            return false;
        }
        output = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

struct RecordedFrames final {
    std::uint16_t count{};
    std::array<std::array<std::byte, kMaximumEncodedFrameBytes>, 2U> bytes{};
    std::array<std::uint16_t, 2U> sizes{};
};

struct RecordedRiskReservation final {
    risk::RiskDelta full_delta;
    std::uint64_t metadata_risk_generation{};
    std::uint64_t metadata_configuration_generation{};
    std::uint64_t metadata_state_generation{};
    std::uint64_t metadata_last_commit_time_ns{};
    risk::ScopeRiskCounters client_before{};
    risk::ScopeRiskCounters client_full{};
    risk::ScopeRiskCounters account_before{};
    risk::ScopeRiskCounters account_full{};
    bool release_required{};
    ParentOrderId release_parent{};
    Quantity unrouted_remainder{};
    Notional release_notional{};
    RemainderAccountingStatus expected_remainder_status{
        RemainderAccountingStatus::NotRequired};
    std::uint64_t full_state_generation{};
    std::uint64_t final_state_generation{};
    risk::ScopeRiskCounters client_final{};
    risk::ScopeRiskCounters account_final{};
};

[[nodiscard]] bool read_risk_command(
    CanonicalReader& reader,
    risk::RiskCommand& output) noexcept {
    std::uint8_t kind{};
    std::span<const std::byte> payload_digest;
    std::uint32_t instrument{};
    std::uint8_t side{};
    std::uint64_t parent{};
    if (!reader.read_u8(kind) || !reader.read_u64(output.request_id) ||
        !reader.read_bytes(output.payload_digest.size(), payload_digest) ||
        !reader.read_u64(output.client_id) ||
        !reader.read_u64(output.account_id) || !reader.read_u64(parent) ||
        !reader.read_u32(instrument) || !reader.read_u8(side) ||
        !reader.read_u64(output.quantity.units) ||
        !reader.read_i64(output.limit_price.units)) {
        return false;
    }
    std::copy(
        payload_digest.begin(), payload_digest.end(), output.payload_digest.begin());
    output.kind = static_cast<risk::RiskCommandKind>(kind);
    output.parent_order_id = ParentOrderId{parent};
    output.instrument = InstrumentId{instrument};
    output.side = static_cast<Side>(side);
    return true;
}

[[nodiscard]] bool read_scope_counters(
    CanonicalReader& reader,
    risk::ScopeRiskCounters& output) noexcept {
    std::uint8_t found{};
    if (!reader.read_u8(found) ||
        !reader.read_u64(output.open_quantity.units) ||
        !reader.read_u64(output.open_notional.units) ||
        !reader.read_u32(output.open_orders) ||
        !reader.read_u32(output.normal_tokens) ||
        !reader.read_u32(output.cancel_tokens)) {
        return false;
    }
    output.found = found != 0U;
    return found <= 1U;
}

[[nodiscard]] RecordedRiskReservation decode_recorded_risk(
    const recorder::DurableRecordView record) {
    RecordedRiskReservation output;
    CanonicalReader reader(record.payload);
    std::uint8_t disposition{};
    std::uint8_t verdict{};
    std::uint8_t release_required{};
    std::uint8_t expected_status{};
    std::uint64_t release_parent{};
    assert(reader.read_u8(disposition));
    assert(reader.read_u8(verdict));
    output.full_delta.disposition =
        static_cast<risk::RiskPlanDisposition>(disposition);
    output.full_delta.verdict = static_cast<risk::RiskVerdict>(verdict);
    assert(read_risk_command(reader, output.full_delta.command));
    assert(reader.read_u64(output.full_delta.command_fingerprint));
    assert(reader.read_u64(output.full_delta.risk_generation));
    assert(reader.read_u64(output.full_delta.configuration_generation));
    assert(reader.read_u64(output.full_delta.kernel_state_generation));
    assert(reader.read_u64(output.full_delta.monotonic_time_ns));
    assert(reader.read_u64(output.full_delta.reservation_quantity.units));
    assert(reader.read_u64(output.full_delta.reservation_notional.units));
    assert(reader.read_u32(output.full_delta.normal_tokens_after));
    assert(reader.read_u32(output.full_delta.cancel_tokens_after));
    assert(reader.read_u64(output.full_delta.normal_refill_time_after));
    assert(reader.read_u64(output.full_delta.cancel_refill_time_after));
    assert(reader.read_u64(output.full_delta.authentication_tag));
    assert(reader.read_u64(output.metadata_risk_generation));
    assert(reader.read_u64(output.metadata_configuration_generation));
    assert(reader.read_u64(output.metadata_state_generation));
    assert(reader.read_u64(output.metadata_last_commit_time_ns));
    assert(read_scope_counters(reader, output.client_before));
    assert(read_scope_counters(reader, output.client_full));
    assert(read_scope_counters(reader, output.account_before));
    assert(read_scope_counters(reader, output.account_full));
    assert(reader.read_u8(release_required));
    assert(reader.read_u64(release_parent));
    assert(reader.read_u64(output.unrouted_remainder.units));
    assert(reader.read_u64(output.release_notional.units));
    assert(reader.read_u8(expected_status));
    assert(reader.read_u64(output.full_state_generation));
    assert(reader.read_u64(output.final_state_generation));
    assert(read_scope_counters(reader, output.client_final));
    assert(read_scope_counters(reader, output.account_final));
    assert(reader.remaining() == 0U);
    assert(release_required <= 1U);
    output.release_required = release_required != 0U;
    output.release_parent = ParentOrderId{release_parent};
    output.expected_remainder_status =
        static_cast<RemainderAccountingStatus>(expected_status);
    return output;
}

[[nodiscard]] RecordedFrames decode_recorded_frames(
    const recorder::DurableRecordView record) {
    RecordedFrames output;
    CanonicalReader reader(record.payload);
    assert(reader.read_u16(output.count));
    assert(output.count == 2U);
    for (std::size_t index = 0U; index < output.count; ++index) {
        assert(reader.skip(4U + 8U + 8U + 8U + 8U + 1U));
        std::uint8_t wire_size{};
        assert(reader.read_u8(wire_size));
        assert(reader.skip(wire_size));
        assert(reader.read_u16(output.sizes[index]));
        std::span<const std::byte> frame_bytes;
        assert(reader.read_bytes(output.sizes[index], frame_bytes));
        assert(output.sizes[index] <= output.bytes[index].size());
        std::copy(
            frame_bytes.begin(), frame_bytes.end(), output.bytes[index].begin());
    }
    std::uint16_t ownership_count{};
    assert(reader.read_u16(ownership_count));
    assert(ownership_count == output.count);
    for (std::size_t index{}; index < ownership_count; ++index) {
        std::uint32_t venue{};
        std::uint64_t child{};
        std::uint64_t venue_session_id{};
        std::uint64_t venue_session_epoch{};
        std::uint64_t ownership_generation{};
        std::uint64_t last_dispatch_attempt_id{};
        std::uint8_t wire_size{};
        assert(reader.read_u32(venue));
        assert(reader.read_u64(child));
        assert(reader.read_u64(venue_session_id));
        assert(reader.read_u64(venue_session_epoch));
        assert(reader.read_u64(ownership_generation));
        assert(reader.read_u64(last_dispatch_attempt_id));
        assert(reader.read_u8(wire_size));
        assert(venue != 0U && child != 0U && venue_session_id != 0U &&
               venue_session_epoch != 0U && ownership_generation != 0U &&
               last_dispatch_attempt_id != 0U && wire_size != 0U &&
               wire_size <= dataplane::kInlineWireIdentityBytes);
        assert(reader.skip(wire_size));
    }
    assert(reader.remaining() == 0U);
    return output;
}

void assert_no_committed_state(const Fixture& fixture) {
    assert(fixture.risk_kernel.metadata().active_parents == 0U);
    assert(fixture.route_kernel.active_parent_count() == 0U);
    assert(fixture.venue_one.normal_size_approx() == 0U);
    assert(fixture.venue_two.normal_size_approx() == 0U);
}

[[nodiscard]] RecordedRiskReservation recorded_risk_reservation(
    Fixture& fixture,
    const RiskDispatchResult& result) {
    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(fixture.tier0.read(result.token.batch_id, batch, token) ==
           recorder::ReadStatus::Ok);
    const auto record = batch.record(1U);
    assert(record.has_value());
    assert(record->type == static_cast<std::uint16_t>(
                               RiskDispatchRecordType::AccountClientRiskDelta));
    assert(record->version == kRiskDispatchRecordVersion);
    return decode_recorded_risk(*record);
}

void assert_scope(
    const risk::ScopeRiskCounters& actual,
    const Quantity quantity,
    const Notional notional,
    const std::uint32_t open_orders) {
    assert(actual.found);
    assert(actual.open_quantity == quantity);
    assert(actual.open_notional == notional);
    assert(actual.open_orders == open_orders);
}

void assert_batch_is_complete(
    Fixture& fixture,
    const RiskDispatchResult& result,
    RecordedFrames& recorded) {
    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(fixture.tier0.read(result.token.batch_id, batch, token) ==
           recorder::ReadStatus::Ok);
    assert(batch.record_count() == 5U);
    const std::array expected_types{
        RiskDispatchRecordType::ClientAccountCommand,
        RiskDispatchRecordType::AccountClientRiskDelta,
        RiskDispatchRecordType::PinnedRouteInputs,
        RiskDispatchRecordType::RouteDecision,
        RiskDispatchRecordType::ExactChildFrames};
    for (std::size_t index = 0U; index < expected_types.size(); ++index) {
        const auto record = batch.record(index);
        assert(record.has_value());
        assert(record->type == static_cast<std::uint16_t>(expected_types[index]));
        assert(record->version == kRiskDispatchRecordVersion);
        assert(!record->payload.empty());
    }
    assert(batch.record(0U)->type >= 0x1201U);
    assert(batch.record(0U)->type != 0x1101U);
    assert(batch.record(1U)->payload.size() > 250U);
    assert(batch.record(2U)->payload.size() > 150U);
    assert(batch.record(3U)->payload.size() > 150U);
    recorded = decode_recorded_frames(*batch.record(4U));
    assert(recorded.sizes[0U] == 45U);
    assert(recorded.sizes[1U] == 45U);
    assert(recorded.bytes[0U][0U] == std::byte{0x4eU});
    assert(recorded.bytes[1U][0U] == std::byte{0x4eU});
    assert(token == result.token);
    const recorder::DispatchAuthorization expected_authorization{1U, 1U, 2U};
    assert(token.dispatch_authorization == expected_authorization);
}

void test_success_one_append_exact_frames_and_conservation() {
    Fixture fixture;
    const auto views = live_views();
    const risk::RiskCommand command = make_command();
    const ParentOrder parent = make_parent();
    RiskDispatchResult result;
    {
        allocation_probe::Scope probe;
        result = fixture.pipeline.try_dispatch(
            command, parent, views, metadata());
        assert(probe.allocations() == 0U);
    }

    assert(result.dispatched());
    assert(result.child_count == 2U);
    assert(result.unrouted_remainder == Quantity{});
    assert(result.remainder_accounting_status ==
           RemainderAccountingStatus::NotRequired);
    assert(fixture.recorder.append_calls == 1U);
    assert(!fixture.pipeline.fail_stopped());
    assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{10U});
    assert(fixture.risk_kernel.client_counters(kClient).open_notional ==
           Notional{1'010U});
    assert(fixture.risk_kernel.account_counters(kAccount).open_orders == 1U);
    assert(fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{10U});
    assert(fixture.route_kernel.instrument_counters(kInstrument).open_parents ==
           1U);
    assert(fixture.route_kernel.instrument_counters(kInstrument).open_children ==
           2U);

    RecordedFrames recorded;
    assert_batch_is_complete(fixture, result, recorded);
    const RecordedRiskReservation reservation =
        recorded_risk_reservation(fixture, result);
    assert(!reservation.release_required);
    assert(reservation.release_parent == parent.id);
    assert(reservation.unrouted_remainder == Quantity{});
    assert(reservation.release_notional == Notional{});
    assert(reservation.expected_remainder_status ==
           RemainderAccountingStatus::NotRequired);
    assert(reservation.full_state_generation ==
           reservation.metadata_state_generation + 1U);
    assert(reservation.final_state_generation ==
           reservation.full_state_generation);
    assert_scope(reservation.client_full, Quantity{10U}, Notional{1'010U}, 1U);
    assert_scope(reservation.client_final, Quantity{10U}, Notional{1'010U}, 1U);
    assert_scope(reservation.account_full, Quantity{10U}, Notional{1'010U}, 1U);
    assert_scope(reservation.account_final, Quantity{10U}, Notional{1'010U}, 1U);

    AuthorizedFrame queued_one;
    AuthorizedFrame queued_two;
    assert(fixture.venue_one.try_pop_normal(queued_one));
    assert(fixture.venue_two.try_pop_normal(queued_two));
    assert(queued_one.frame.size == recorded.sizes[0U]);
    assert(queued_two.frame.size == recorded.sizes[1U]);
    assert(std::equal(
        queued_one.frame.bytes().begin(),
        queued_one.frame.bytes().end(),
        recorded.bytes[0U].begin()));
    assert(std::equal(
        queued_two.frame.bytes().begin(),
        queued_two.frame.bytes().end(),
        recorded.bytes[1U].begin()));

    assert(fixture.route_kernel.release_child(parent.id, kVenueOne) ==
           ReleaseStatus::Released);
    assert(fixture.route_kernel.release_child(parent.id, kVenueTwo) ==
           ReleaseStatus::Released);
    assert(fixture.risk_kernel.terminal(parent.id) ==
           risk::RiskAccountingStatus::Applied);
    assert(fixture.route_kernel.active_parent_count() == 0U);
    assert(fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{});
    assert(fixture.route_kernel.instrument_counters(kInstrument).open_parents ==
           0U);
    assert(fixture.route_kernel.instrument_counters(kInstrument).open_children ==
           0U);
    assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{});
    assert(fixture.risk_kernel.client_counters(kClient).open_notional ==
           Notional{});
    assert(fixture.risk_kernel.client_counters(kClient).open_orders == 0U);
    assert(fixture.risk_kernel.account_counters(kAccount).open_quantity ==
           Quantity{});
    assert(fixture.risk_kernel.account_counters(kAccount).open_notional ==
           Notional{});
    assert(fixture.risk_kernel.account_counters(kAccount).open_orders == 0U);
}

void test_partial_ioc_releases_unrouted_risk_and_replays_exact_durable_bytes() {
    Fixture fixture;
    const auto views = live_views();
    const risk::RiskCommand command = make_command(101U, 1U, 12U);
    const ParentOrder parent = make_parent(1U, 12U);
    RiskDispatchResult result;
    {
        allocation_probe::Scope probe;
        result = fixture.pipeline.try_dispatch(
            command, parent, views, metadata());
        assert(probe.allocations() == 0U);
    }

    assert(result.dispatched());
    assert(result.child_count == 2U);
    assert(result.unrouted_remainder == Quantity{2U});
    assert(result.remainder_accounting_status ==
           RemainderAccountingStatus::Applied);
    assert(fixture.recorder.append_calls == 1U);
    assert(fixture.risk_kernel.metadata().active_parents == 1U);
    assert_scope(
        fixture.risk_kernel.client_counters(kClient),
        Quantity{10U},
        Notional{1'010U},
        1U);
    assert_scope(
        fixture.risk_kernel.account_counters(kAccount),
        Quantity{10U},
        Notional{1'010U},
        1U);
    assert(fixture.risk_kernel.active_parent(parent.id).remaining_quantity ==
           Quantity{10U});
    assert(fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{10U});

    const RecordedRiskReservation reservation =
        recorded_risk_reservation(fixture, result);
    assert(reservation.full_delta.committable());
    assert(reservation.full_delta.command.parent_order_id == parent.id);
    assert(reservation.full_delta.reservation_quantity == Quantity{12U});
    assert(reservation.full_delta.reservation_notional == Notional{1'212U});
    assert(reservation.release_required);
    assert(reservation.release_parent == parent.id);
    assert(reservation.unrouted_remainder == Quantity{2U});
    assert(reservation.release_notional == Notional{202U});
    assert(reservation.expected_remainder_status ==
           RemainderAccountingStatus::Applied);
    assert(reservation.full_state_generation ==
           reservation.full_delta.kernel_state_generation + 1U);
    assert(reservation.final_state_generation ==
           reservation.full_state_generation + 1U);
    assert_scope(reservation.client_before, Quantity{}, Notional{}, 0U);
    assert_scope(reservation.client_full, Quantity{12U}, Notional{1'212U}, 1U);
    assert_scope(reservation.client_final, Quantity{10U}, Notional{1'010U}, 1U);
    assert_scope(reservation.account_before, Quantity{}, Notional{}, 0U);
    assert_scope(reservation.account_full, Quantity{12U}, Notional{1'212U}, 1U);
    assert_scope(reservation.account_final, Quantity{10U}, Notional{1'010U}, 1U);

    const auto recovered_accounts = account_configuration();
    const auto recovered_clients = client_configuration();
    const std::array<InstrumentId, 0U> recovered_restricted{};
    risk::RiskKernel recovered(
        make_risk_configuration(),
        recovered_accounts,
        recovered_clients,
        recovered_restricted);
    assert(recovered.begin_recovery());
    assert(recovered.restore_recovered_delta(reservation.full_delta) ==
           risk::RiskRecoveryImportStatus::Imported);
    assert(recovered.apply_recovered_release(
               reservation.release_parent, reservation.unrouted_remainder) ==
           risk::RiskAccountingStatus::Applied);
    assert_scope(
        recovered.client_counters(kClient),
        Quantity{10U},
        Notional{1'010U},
        1U);
    assert_scope(
        recovered.account_counters(kAccount),
        Quantity{10U},
        Notional{1'010U},
        1U);
    assert(recovered.metadata().kernel_state_generation ==
           reservation.final_state_generation);
    assert(recovered.complete_recovery());

    assert(fixture.route_kernel.release_child(parent.id, kVenueOne) ==
           ReleaseStatus::Released);
    assert(fixture.risk_kernel.release(parent.id, Quantity{4U}) ==
           risk::RiskAccountingStatus::Applied);
    assert(fixture.route_kernel.release_child(parent.id, kVenueTwo) ==
           ReleaseStatus::Released);
    assert(fixture.risk_kernel.release(parent.id, Quantity{6U}) ==
           risk::RiskAccountingStatus::Applied);
    assert(fixture.route_kernel.active_parent_count() == 0U);
    assert(fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{});
    assert_scope(
        fixture.risk_kernel.client_counters(kClient),
        Quantity{},
        Notional{},
        0U);
    assert_scope(
        fixture.risk_kernel.account_counters(kAccount),
        Quantity{},
        Notional{},
        0U);
    assert(fixture.risk_kernel.metadata().active_parents == 0U);
}

void restore_risk_state_generation(
    Fixture& fixture,
    const std::uint64_t state_generation) {
    std::array<risk::RecoveredAccountRiskState, 1U> accounts{};
    std::array<risk::RecoveredClientRiskState, 1U> clients{};
    std::array<risk::RecoveredParentRiskState, 4U> parents{};
    std::array<risk::RecoveredDedupeRiskState, 4U> dedupe{};
    const risk::RiskRecoveryExportResult exported =
        fixture.risk_kernel.export_recovery_snapshot(
            accounts, clients, parents, dedupe);
    assert(exported.exported());
    risk::RiskRecoveryKernelState state =
        fixture.risk_kernel.recovery_kernel_state();
    state.kernel_state_generation = state_generation;
    assert(fixture.risk_kernel.begin_recovery());
    assert(fixture.risk_kernel.import_recovery_snapshot(
               state,
               std::span{accounts}.first(exported.account_count),
               std::span{clients}.first(exported.client_count),
               std::span{parents}.first(exported.active_parent_count),
               std::span{dedupe}.first(exported.dedupe_count)) ==
           risk::RiskRecoveryImportStatus::Imported);
    assert(fixture.risk_kernel.complete_recovery());
}

void test_partial_ioc_generation_exhaustion_is_rejected_before_durability() {
    Fixture fixture;
    restore_risk_state_generation(
        fixture, std::numeric_limits<std::uint64_t>::max() - 1U);
    const auto views = live_views();
    const RiskDispatchResult result = fixture.pipeline.try_dispatch(
        make_command(101U, 1U, 12U),
        make_parent(1U, 12U),
        views,
        metadata());
    assert(result.status ==
           RiskDispatchStatus::RemainderAccountingPreflightRejected);
    assert(result.unrouted_remainder == Quantity{2U});
    assert(result.remainder_accounting_status ==
           RemainderAccountingStatus::GenerationExhausted);
    assert(fixture.recorder.append_calls == 0U);
    assert(fixture.risk_kernel.metadata().kernel_state_generation ==
           std::numeric_limits<std::uint64_t>::max() - 1U);
    assert_no_committed_state(fixture);
}

void test_duplicate_conflict_and_loop_reject_before_append() {
    Fixture fixture;
    const auto views = live_views();
    const risk::RiskCommand command = make_command();
    const ParentOrder parent = make_parent();
    const RiskDispatchResult first = fixture.pipeline.try_dispatch(
        command, parent, views, metadata());
    assert(first.dispatched());
    assert(fixture.recorder.append_calls == 1U);

    const RiskDispatchResult replay = fixture.pipeline.try_dispatch(
        command, parent, views, metadata(92U, 1'060U));
    assert(replay.status == RiskDispatchStatus::RiskDuplicateReplay);
    assert(fixture.recorder.append_calls == 1U);

    risk::RiskCommand conflict = command;
    conflict.payload_digest[0U] ^= std::byte{0xffU};
    const RiskDispatchResult conflict_result = fixture.pipeline.try_dispatch(
        conflict, parent, views, metadata(93U, 1'070U));
    assert(conflict_result.status == RiskDispatchStatus::RiskDuplicateConflict);
    assert(fixture.recorder.append_calls == 1U);

    const risk::RiskCommand loop = make_command(102U);
    const RiskDispatchResult loop_result = fixture.pipeline.try_dispatch(
        loop, parent, views, metadata(94U, 1'080U));
    assert(loop_result.status == RiskDispatchStatus::RiskRejected);
    assert(loop_result.risk_verdict == risk::RiskVerdict::DuplicateParent);
    assert(fixture.recorder.append_calls == 1U);
    assert(fixture.route_kernel.active_parent_count() == 1U);
    assert(fixture.venue_one.normal_size_approx() == 1U);
    assert(fixture.venue_two.normal_size_approx() == 1U);
}

void test_all_preappend_failures_leave_both_kernels_unchanged() {
    const auto views = live_views();
    {
        Fixture fixture;
        const ParentOrder too_large = make_parent(1U, 101U);
        const risk::RiskCommand command = make_command(101U, 1U, 101U);
        const RiskDispatchResult result = fixture.pipeline.try_dispatch(
            command, too_large, views, metadata());
        assert(result.status == RiskDispatchStatus::RiskRejected);
        assert(result.risk_verdict == risk::RiskVerdict::OrderQuantityExceeded);
        assert(fixture.recorder.append_calls == 0U);
        assert_no_committed_state(fixture);
    }
    {
        Fixture fixture;
        const ParentOrder no_price = make_parent(1U, 10U, Price{99});
        const risk::RiskCommand command =
            make_command(101U, 1U, 10U, Price{99});
        const RiskDispatchResult result = fixture.pipeline.try_dispatch(
            command, no_price, views, metadata());
        assert(result.status == RiskDispatchStatus::RouteRejected);
        assert(fixture.recorder.append_calls == 0U);
        assert_no_committed_state(fixture);
        assert(fixture.risk_kernel.metadata().dedupe_entries == 0U);
    }
    {
        Fixture fixture;
        saturate(fixture.venue_one);
        const RiskDispatchResult result = fixture.pipeline.try_dispatch(
            make_command(), make_parent(), views, metadata());
        assert(result.status ==
               RiskDispatchStatus::NormalQueueCapacityUnavailable);
        assert(fixture.recorder.append_calls == 0U);
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
        assert(fixture.venue_two.normal_size_approx() == 0U);
    }
    {
        Fixture fixture;
        fixture.encoder_one.fail = true;
        const RiskDispatchResult result = fixture.pipeline.try_dispatch(
            make_command(), make_parent(), views, metadata());
        assert(result.status == RiskDispatchStatus::FrameEncodingFailed);
        assert(result.encode_status == FrameEncodeStatus::Failed);
        assert(fixture.recorder.append_calls == 0U);
        assert_no_committed_state(fixture);
    }
}

void assert_recovery_batch_readable(
    Fixture& fixture,
    const RiskDispatchResult& result) {
    assert(result.recovery.required);
    assert(result.recovery.durable_locator_complete);
    assert(result.recovery.durable_batch_id == result.token.batch_id);
    assert(result.recovery.producer_sequence == result.token.producer_sequence);
    assert(result.recovery.batch_hash == result.token.batch_hash);
    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(fixture.tier0.read(
               result.recovery.durable_batch_id, batch, token) ==
           recorder::ReadStatus::Ok);
    assert(batch.record_count() == 5U);
    assert(token == result.token);
}

void test_posttoken_risk_commit_mismatch_is_durable_only_failstop() {
    Fixture fixture;
    fixture.recorder.fault = RecorderFault::MutateRisk;
    const auto views = live_views();
    const RiskDispatchResult result = fixture.pipeline.try_dispatch(
        make_command(), make_parent(), views, metadata());
    assert(fixture.recorder.mutation_succeeded);
    assert(result.status == RiskDispatchStatus::RiskCommitRejectedFailStop);
    assert(result.risk_commit_status == risk::RiskCommitStatus::StaleDelta);
    assert(result.recovery.applied_phase == RecoveryCommitPhase::DurableOnly);
    assert(fixture.pipeline.fail_stopped());
    assert_recovery_batch_readable(fixture, result);
    assert_no_committed_state(fixture);

    const RiskDispatchResult stopped = fixture.pipeline.try_dispatch(
        make_command(), make_parent(), views, metadata(92U, 1'060U));
    assert(stopped.status == RiskDispatchStatus::FailStopped);
    assert(stopped.recovery.required);
    assert(fixture.recorder.append_calls == 1U);
}

void test_posttoken_route_commit_mismatch_reports_risk_committed() {
    Fixture fixture;
    fixture.recorder.fault = RecorderFault::MutateRoute;
    const auto views = live_views();
    const RiskDispatchResult result = fixture.pipeline.try_dispatch(
        make_command(), make_parent(), views, metadata());
    assert(fixture.recorder.mutation_succeeded);
    assert(result.status == RiskDispatchStatus::RouteCommitRejectedFailStop);
    assert(result.risk_commit_status == risk::RiskCommitStatus::Committed);
    assert(result.route_commit_status == CommitStatus::StalePlan);
    assert(result.recovery.applied_phase == RecoveryCommitPhase::RiskCommitted);
    assert(fixture.pipeline.fail_stopped());
    assert_recovery_batch_readable(fixture, result);
    assert(fixture.risk_kernel.metadata().active_parents == 1U);
    assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{10U});
    assert(fixture.route_kernel.active_parent_count() == 0U);
    assert(fixture.venue_one.normal_size_approx() == 0U);
    assert(fixture.venue_two.normal_size_approx() == 0U);
    assert(fixture.risk_kernel.terminal(ParentOrderId{1U}) ==
           risk::RiskAccountingStatus::Applied);
}

void test_partial_route_commit_mismatch_reports_remainder_released() {
    Fixture fixture;
    fixture.recorder.fault = RecorderFault::MutateRoute;
    const auto views = live_views();
    const RiskDispatchResult result = fixture.pipeline.try_dispatch(
        make_command(101U, 1U, 12U),
        make_parent(1U, 12U),
        views,
        metadata());
    assert(fixture.recorder.mutation_succeeded);
    assert(result.status == RiskDispatchStatus::RouteCommitRejectedFailStop);
    assert(result.risk_commit_status == risk::RiskCommitStatus::Committed);
    assert(result.route_commit_status == CommitStatus::StalePlan);
    assert(result.unrouted_remainder == Quantity{2U});
    assert(result.remainder_accounting_status ==
           RemainderAccountingStatus::Applied);
    assert(result.recovery.applied_phase ==
           RecoveryCommitPhase::RiskAndRemainderReleased);
    assert(result.recovery.unrouted_remainder == Quantity{2U});
    assert(result.recovery.remainder_accounting_status ==
           RemainderAccountingStatus::Applied);
    assert(fixture.pipeline.fail_stopped());
    assert_recovery_batch_readable(fixture, result);
    assert_scope(
        fixture.risk_kernel.client_counters(kClient),
        Quantity{10U},
        Notional{1'010U},
        1U);
    assert(fixture.risk_kernel.active_parent(ParentOrderId{1U})
               .remaining_quantity == Quantity{10U});
    assert(fixture.route_kernel.active_parent_count() == 0U);
    assert(fixture.venue_one.normal_size_approx() == 0U);
    assert(fixture.venue_two.normal_size_approx() == 0U);
    assert(fixture.risk_kernel.terminal(ParentOrderId{1U}) ==
           risk::RiskAccountingStatus::Applied);
}

void test_queue_ownership_violation_latches_both_commits() {
    Fixture fixture;
    fixture.recorder.fault = RecorderFault::SaturateVenueOne;
    const auto views = live_views();
    const RiskDispatchResult result = fixture.pipeline.try_dispatch(
        make_command(), make_parent(), views, metadata());
    assert(fixture.recorder.mutation_succeeded);
    assert(result.status ==
           RiskDispatchStatus::QueueOwnershipInvariantFailStop);
    assert(result.recovery.required);
    assert(result.recovery.applied_phase ==
           RecoveryCommitPhase::RiskAndRouteCommitted);
    assert(fixture.pipeline.fail_stopped());
    assert(fixture.risk_kernel.metadata().active_parents == 1U);
    assert(fixture.route_kernel.active_parent_count() == 1U);
    assert(fixture.venue_one.normal_size_approx() ==
           kNormalVenueQueueCapacity);
    assert(fixture.venue_two.normal_size_approx() == 0U);
    assert_recovery_batch_readable(fixture, result);
}

void test_missing_token_is_explicit_incomplete_locator_failstop() {
    const auto candidates = candidate_table();
    const CoreConfiguration route_configuration = make_route_configuration();
    RouteKernel route_kernel(
        route_configuration,
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric);
    const auto accounts = account_configuration();
    const auto clients = client_configuration();
    const std::array<InstrumentId, 0U> restricted{};
    risk::RiskKernel risk_kernel(
        make_risk_configuration(), accounts, clients, restricted);
    MissingTokenRecorder recorder;
    VenueTxQueues venue_one;
    VenueTxQueues venue_two;
    DeterministicEncoder encoder_one;
    DeterministicEncoder encoder_two;
    FixedVenueRegistry registry;
    assert(registry.register_venue(kVenueOne, venue_one, encoder_one) ==
           VenueRegistrationStatus::Registered);
    assert(registry.register_venue(kVenueTwo, venue_two, encoder_two) ==
           VenueRegistrationStatus::Registered);
    assert(registry.seal());
    RiskDispatchPipeline pipeline(
        risk_kernel,
        route_kernel,
        recorder,
        registry,
        {kLeadershipEpoch,
         kConfigurationGeneration,
         recorder::DurabilityDomain::VolatileSoftwareReference});
    const auto views = live_views();
    const RiskDispatchResult result = pipeline.try_dispatch(
        make_command(), make_parent(), views, metadata());
    assert(result.status == RiskDispatchStatus::DurableTokenUnavailable);
    assert(result.recovery.required);
    assert(!result.recovery.durable_locator_complete);
    assert(result.recovery.durable_batch_id == 0U);
    assert(result.recovery.producer_sequence == metadata().producer_sequence);
    assert(result.recovery.applied_phase == RecoveryCommitPhase::DurableOnly);
    assert(std::all_of(
        result.recovery.batch_hash.begin(),
        result.recovery.batch_hash.end(),
        [](const std::byte value) noexcept { return value == std::byte{}; }));
    assert(pipeline.fail_stopped());
    assert(recorder.append_calls == 1U);
    assert(risk_kernel.metadata().active_parents == 0U);
    assert(route_kernel.active_parent_count() == 0U);
    assert(venue_one.normal_size_approx() == 0U);
    assert(venue_two.normal_size_approx() == 0U);
    const RiskDispatchResult stopped = pipeline.try_dispatch(
        make_command(), make_parent(), views, metadata(92U, 1'060U));
    assert(stopped.status == RiskDispatchStatus::FailStopped);
    assert(recorder.append_calls == 1U);
}

void test_token_domain_and_dual_durable_failures_latch_recovery() {
    const auto views = live_views();
    {
        Fixture fixture(recorder::DurabilityDomain::HardwarePersistent);
        const RiskDispatchResult result = fixture.pipeline.try_dispatch(
            make_command(), make_parent(), views, metadata());
        assert(result.status == RiskDispatchStatus::DurableTokenRejected);
        assert(result.recovery.required);
        assert(result.recovery.applied_phase ==
               RecoveryCommitPhase::DurableOnly);
        assert_recovery_batch_readable(fixture, result);
        assert_no_committed_state(fixture);
    }
    {
        ha::ReplicationGate gate(1U, ha::ReplicationPolicy::DualDurable);
        assert(gate.install_authenticated_lease(
            {1U, kLeadershipEpoch, 1U, 10'000U}));
        Fixture fixture(
            recorder::DurabilityDomain::VolatileSoftwareReference,
            &gate,
            nullptr);
        const RiskDispatchResult result = fixture.pipeline.try_dispatch(
            make_command(), make_parent(), views, metadata());
        assert(result.status == RiskDispatchStatus::DualDurabilityRejected);
        assert(result.fence_error ==
               ha::DispatchFenceError::RemoteRecordNotDurable);
        assert(result.recovery.required);
        assert(result.recovery.applied_phase ==
               RecoveryCommitPhase::DurableOnly);
        assert_recovery_batch_readable(fixture, result);
        assert_no_committed_state(fixture);
    }
}

}  // namespace

int main() {
    test_success_one_append_exact_frames_and_conservation();
    test_partial_ioc_releases_unrouted_risk_and_replays_exact_durable_bytes();
    test_partial_ioc_generation_exhaustion_is_rejected_before_durability();
    test_duplicate_conflict_and_loop_reject_before_append();
    test_all_preappend_failures_leave_both_kernels_unchanged();
    test_posttoken_risk_commit_mismatch_is_durable_only_failstop();
    test_posttoken_route_commit_mismatch_reports_risk_committed();
    test_partial_route_commit_mismatch_reports_remainder_released();
    test_queue_ownership_violation_latches_both_commits();
    test_missing_token_is_explicit_incomplete_locator_failstop();
    test_token_domain_and_dual_durable_failures_latch_recovery();
    return 0;
}
