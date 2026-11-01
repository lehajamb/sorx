#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/recovery/risk_dispatch_recovery.hpp"

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
using namespace sorx::recovery;

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

[[nodiscard]] CoreConfiguration route_configuration() {
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

[[nodiscard]] risk::RiskLimits risk_limits() noexcept {
    return risk::RiskLimits{Quantity{100U},
                            Notional{1'000'000U},
                            Quantity{1'000U},
                            Notional{10'000'000U},
                            100U,
                            Price{1},
                            Price{1'000'000}};
}

[[nodiscard]] std::array<risk::AccountRiskConfig, 1U> accounts() noexcept {
    return {risk::AccountRiskConfig{
        kAccount, risk::RiskPermission::NewAndCancel, risk_limits()}};
}

[[nodiscard]] std::array<risk::ClientRiskConfig, 1U> clients() noexcept {
    return {risk::ClientRiskConfig{
        kClient,
        kAccount,
        risk::RiskPermission::NewAndCancel,
        risk_limits(),
        {100U, 1U, 1'000U},
        {20U, 1U, 1'000U}}};
}

[[nodiscard]] risk::RiskKernelConfig risk_configuration() noexcept {
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
    const Quantity quantity) {
    md::BookView book;
    book.venue = venue;
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = kInstrument;
    assert(book.push_bid(
        md::Level{Price{ask.units - 2}, Quantity{10U}, 1U}));
    assert(book.push_ask(md::Level{ask, quantity, 1U}));
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

[[nodiscard]] std::array<BookSlotView, 2U> views() {
    return {live_view(kVenueOne, Price{100}, Quantity{4U}),
            live_view(kVenueTwo, Price{101}, Quantity{6U})};
}

[[nodiscard]] risk::PayloadDigest digest() noexcept {
    risk::PayloadDigest result{};
    for (std::size_t index{}; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(101U + index);
    }
    return result;
}

[[nodiscard]] ParentOrder parent() noexcept {
    return ParentOrder{ParentOrderId{1U},
                       kInstrument,
                       Side::Buy,
                       Quantity{12U},
                       Price{101},
                       RoutingIntent::AggressiveIOC};
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
        0U};
}

[[nodiscard]] policy::VenueFactPin fact_pin(
    const BookSlotView& view) noexcept {
    return policy::VenueFactPin{
        view.venue,
        policy::VenueFactVersions{
            view.adjacency_generation,
            view.book.view_generation,
            1U,
            kRiskGeneration,
            view.throttle_generation,
            view.tx_credit_generation,
            1U,
            policy_versions().fee_generation,
            policy_versions().metric_generation}};
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
        500U};
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
        view.venue == kVenueOne ? 1U : 2U};
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
        500U};
    output.fact_pin = policy::VenueFactPin{view.venue, {}};
    output.exclusion_reason =
        policy::PolicyExclusionReason::RouteVersionMissing;
    output.applied = true;
    assert(output.valid_shape());
    return output;
}

void apply_adaptive_fixture(
    std::array<BookSlotView, 2U>& market,
    const bool exclude_first) noexcept {
    constexpr std::uint64_t decision_time_ns = 1'050U;
    const ParentOrder order = parent();
    market[0U].adaptive_policy = exclude_first
        ? excluded_overlay(order, market[0U], decision_time_ns)
        : eligible_overlay(order, market[0U], decision_time_ns, 0U);
    market[1U].adaptive_policy =
        eligible_overlay(
            order,
            market[1U],
            decision_time_ns,
            exclude_first ? 0U : 1U);
}

[[nodiscard]] risk::RiskCommand command() noexcept {
    return risk::RiskCommand{risk::RiskCommandKind::NewOrder,
                             101U,
                             digest(),
                             kClient,
                             kAccount,
                             ParentOrderId{1U},
                             kInstrument,
                             Side::Buy,
                             Quantity{12U},
                             Price{101}};
}

void put_u32(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    output[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    output[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    output[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    output[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

void put_u16(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    output[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] std::uint16_t get_u16(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept {
    assert(offset + 2U <= input.size());
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[offset]) << 8U) |
        std::to_integer<std::uint16_t>(input[offset + 1U]));
}

[[nodiscard]] std::uint32_t get_u32(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept {
    assert(offset + 4U <= input.size());
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(input[offset])) << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(input[offset + 1U])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(input[offset + 2U])) << 8U) |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(input[offset + 3U]));
}

[[nodiscard]] std::uint64_t get_u64(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept {
    assert(offset + 8U <= input.size());
    std::uint64_t value{};
    for (std::size_t index{}; index < 8U; ++index) {
        value = (value << 8U) |
            std::to_integer<std::uint8_t>(input[offset + index]);
    }
    return value;
}

void put_u64(
    std::span<std::byte> output,
    std::size_t offset,
    std::uint64_t value) noexcept;

[[nodiscard]] constexpr std::uint32_t rotate_right(
    const std::uint32_t value,
    const unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class TestSha256 final {
public:
    void update(std::span<const std::byte> bytes) noexcept {
        total_bytes_ += bytes.size();
        while (!bytes.empty()) {
            const std::size_t available = block_.size() - block_size_;
            const std::size_t copied = std::min(available, bytes.size());
            std::copy_n(bytes.begin(), copied, block_.begin() + block_size_);
            block_size_ += copied;
            bytes = bytes.subspan(copied);
            if (block_size_ == block_.size()) {
                transform(block_);
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] recorder::HashChainDigest finish() noexcept {
        const std::uint64_t bit_length =
            static_cast<std::uint64_t>(total_bytes_) * 8U;
        block_[block_size_++] = std::byte{0x80U};
        if (block_size_ > 56U) {
            std::fill(block_.begin() + block_size_, block_.end(), std::byte{});
            transform(block_);
            block_size_ = 0U;
        }
        std::fill(
            block_.begin() + block_size_,
            block_.begin() + 56U,
            std::byte{});
        put_u64(block_, 56U, bit_length);
        transform(block_);
        recorder::HashChainDigest output{};
        for (std::size_t index{}; index < state_.size(); ++index) {
            put_u32(output, index * 4U, state_[index]);
        }
        return output;
    }

private:
    void transform(const std::array<std::byte, 64U>& block) noexcept {
        static constexpr std::array<std::uint32_t, 64U> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::array<std::uint32_t, 64U> words{};
        for (std::size_t index{}; index < 16U; ++index) {
            words[index] = get_u32(block, index * 4U);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t s0 =
                rotate_right(words[index - 15U], 7U) ^
                rotate_right(words[index - 15U], 18U) ^
                (words[index - 15U] >> 3U);
            const std::uint32_t s1 =
                rotate_right(words[index - 2U], 17U) ^
                rotate_right(words[index - 2U], 19U) ^
                (words[index - 2U] >> 10U);
            words[index] =
                words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        std::uint32_t a = state_[0U];
        std::uint32_t b = state_[1U];
        std::uint32_t c = state_[2U];
        std::uint32_t d = state_[3U];
        std::uint32_t e = state_[4U];
        std::uint32_t f = state_[5U];
        std::uint32_t g = state_[6U];
        std::uint32_t h = state_[7U];
        for (std::size_t index{}; index < words.size(); ++index) {
            const std::uint32_t upper_sigma1 =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                rotate_right(e, 25U);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + upper_sigma1 + choice + constants[index] + words[index];
            const std::uint32_t upper_sigma0 =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = upper_sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0U] += a;
        state_[1U] += b;
        state_[2U] += c;
        state_[3U] += d;
        state_[4U] += e;
        state_[5U] += f;
        state_[6U] += g;
        state_[7U] += h;
    }

    std::array<std::uint32_t, 8U> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::byte, 64U> block_{};
    std::size_t total_bytes_{};
    std::size_t block_size_{};
};

void put_u64(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index{}; index < 8U; ++index) {
        output[offset + index] = static_cast<std::byte>(
            (value >> static_cast<unsigned>((7U - index) * 8U)) & 0xffU);
    }
}

class Encoder final : public IChildFrameEncoder {
public:
    [[nodiscard]] FrameEncodeResult encode_new(
        const ParentOrder& order,
        const ChildAllocation& allocation,
        const PlannedChild& child,
        const std::span<std::byte> destination) noexcept override {
        constexpr std::uint16_t size = 37U;
        if (destination.size() < size) {
            return {FrameEncodeStatus::BufferTooSmall, 0U};
        }
        destination[0U] = std::byte{0x4eU};
        put_u32(destination, 1U, allocation.venue.value());
        put_u64(destination, 5U, order.id.value());
        put_u64(destination, 13U, child.child_order_id.value());
        put_u64(destination, 21U, child.dispatch_attempt_id);
        put_u64(destination, 29U, allocation.venue_quantity.units);
        return {FrameEncodeStatus::Encoded, size};
    }
};

struct Producer final {
    Producer()
        : candidates(candidate_table()),
          configuration(route_configuration()),
          route_kernel(
              configuration,
              *candidates,
              AdaptivePolicyMode::Optional,
              WireIdentityMode::ReferenceGeneric),
          account_configs(accounts()),
          client_configs(clients()),
          risk_kernel(
              risk_configuration(),
              account_configs,
              client_configs,
              restricted),
          tier0({64U, 4U, 4U, false}),
          pipeline(
              risk_kernel,
              route_kernel,
              tier0,
              registry,
              {kLeadershipEpoch,
               kConfigurationGeneration,
               recorder::DurabilityDomain::VolatileSoftwareReference}) {
        assert(registry.register_venue(kVenueOne, queues_one, encoder_one) ==
               VenueRegistrationStatus::Registered);
        assert(registry.register_venue(kVenueTwo, queues_two, encoder_two) ==
               VenueRegistrationStatus::Registered);
        assert(registry.seal());
    }

    std::shared_ptr<const CandidateTable> candidates;
    CoreConfiguration configuration;
    RouteKernel route_kernel;
    std::array<risk::AccountRiskConfig, 1U> account_configs;
    std::array<risk::ClientRiskConfig, 1U> client_configs;
    std::array<InstrumentId, 0U> restricted{};
    risk::RiskKernel risk_kernel;
    recorder::InMemoryMirroredTier0 tier0;
    VenueTxQueues queues_one;
    VenueTxQueues queues_two;
    Encoder encoder_one;
    Encoder encoder_two;
    FixedVenueRegistry registry;
    RiskDispatchPipeline pipeline;
};

[[nodiscard]] RiskDispatchRecoveryConfiguration recovery_configuration()
    noexcept {
    RiskDispatchRecoveryConfiguration output{
        16U,
        16U,
        4U,
        4U,
        32U,
        kLeadershipEpoch,
        kConfigurationGeneration,
        1U,
        kRiskGeneration,
        recorder::DurabilityDomain::VolatileSoftwareReference,
        {}};
    output.checkpoint_trust_policy =
        RiskDispatchCheckpointTrustPolicy::ReferenceSelfChecked;
    return output;
}

struct RecoveryFixture final {
    explicit RecoveryFixture(
        const RiskDispatchRecoveryConfiguration recovery =
            recovery_configuration())
        : candidates(candidate_table()),
          configuration(route_configuration()),
          route_kernel(
              configuration,
              *candidates,
              AdaptivePolicyMode::Optional,
              WireIdentityMode::ReferenceGeneric),
          account_configs(accounts()),
          client_configs(clients()),
          risk_kernel(
              risk_configuration(),
              account_configs,
              client_configs,
              restricted),
          ledger(risk_kernel, route_kernel, recovery) {
        assert(ledger.ready());
    }

    std::shared_ptr<const CandidateTable> candidates;
    CoreConfiguration configuration;
    RouteKernel route_kernel;
    std::array<risk::AccountRiskConfig, 1U> account_configs;
    std::array<risk::ClientRiskConfig, 1U> client_configs;
    std::array<InstrumentId, 0U> restricted{};
    risk::RiskKernel risk_kernel;
    RiskDispatchRecoveryLedger ledger;
};

struct CapturedBatch final {
    recorder::DurableBatch batch{};
    recorder::DurableToken token{};
};

inline constexpr std::size_t kLegacyV1RiskRecordBytes = 308U;

[[nodiscard]] std::size_t legacy_frame_prefix_size(
    const std::span<const std::byte> payload) noexcept {
    if (payload.size() < 2U) {
        return 0U;
    }
    const std::uint16_t count = get_u16(payload, 0U);
    std::size_t offset = 2U;
    for (std::size_t index{}; index < count; ++index) {
        constexpr std::size_t kFixedThroughDispatchClass =
            4U + (4U * 8U) + 1U;
        if (payload.size() - offset < kFixedThroughDispatchClass + 1U) {
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
        const std::size_t frame_size = get_u16(payload, offset);
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
    const std::uint16_t pin_count = get_u16(input, 0U);
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
        ++source_offset;  // v1 had no adaptive-overlay byte.
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
            (has_book != 0U && !copy_bytes(83U))) {
            return false;
        }
    }
    if (!copy_bytes(input.size() - source_offset)) {
        return false;
    }
    output_size = destination_offset;
    return true;
}

void assert_recovered(const RecoveryFixture& fixture);

[[nodiscard]] CapturedBatch produce(
    const bool conflicting_payload = false,
    const bool adaptive_policy = false,
    const bool exclude_first = false) {
    Producer producer;
    auto market = views();
    if (adaptive_policy) {
        apply_adaptive_fixture(market, exclude_first);
    }
    risk::RiskCommand order = command();
    if (conflicting_payload) {
        order.payload_digest[0U] ^= std::byte{0x5aU};
    }
    const RiskDispatchResult dispatched = producer.pipeline.try_dispatch(
        order,
        parent(),
        market,
        {91U, 1'050U, 1'060U, 1'055U});
    assert(dispatched.dispatched());
    assert(dispatched.unrouted_remainder ==
           Quantity{exclude_first ? 6U : 2U});
    CapturedBatch captured;
    assert(producer.tier0.read(
               dispatched.token.batch_id, captured.batch, captured.token) ==
           recorder::ReadStatus::Ok);
    return captured;
}

[[nodiscard]] CapturedBatch make_legacy_batch(
    const CapturedBatch& source,
    const std::uint16_t version) {
    assert(version == 1U || version == 2U);
    recorder::DurableBatch legacy(
        source.batch.lane(), source.batch.producer_sequence());
    legacy.set_leadership_epoch(source.batch.leadership_epoch());
    legacy.set_configuration_generation(
        source.batch.configuration_generation());
    assert(legacy.set_dispatch_authorization(
        source.batch.dispatch_authorization()));
    assert(legacy.set_authorization_envelope_version(
        recorder::AuthorizationEnvelopeVersion::LegacyRangeV0));

    std::array<std::byte, recorder::kMaximumDurableBatchBytes> command_bytes{};
    std::array<std::byte, recorder::kMaximumDurableBatchBytes> pinned_bytes{};
    std::size_t pinned_size{};
    for (std::size_t index{}; index < source.batch.record_count(); ++index) {
        const auto record = source.batch.record(index);
        assert(record.has_value());
        std::span<const std::byte> payload = record->payload;
        if (index == 0U) {
            assert(payload.size() <= command_bytes.size() &&
                   payload.size() >= 6U);
            std::copy(payload.begin(), payload.end(), command_bytes.begin());
            put_u16(
                command_bytes,
                4U,
                version);
            payload = {command_bytes.data(), payload.size()};
        } else if (index == 1U && version == 1U) {
            assert(payload.size() > kLegacyV1RiskRecordBytes);
            payload = payload.first(kLegacyV1RiskRecordBytes);
        } else if (index == 2U && version == 1U) {
            assert(make_legacy_unapplied_pinned_payload(
                payload, pinned_bytes, pinned_size));
            assert(pinned_size + 2U == payload.size());
            payload = {pinned_bytes.data(), pinned_size};
        } else if (index == 4U) {
            const std::size_t prefix = legacy_frame_prefix_size(payload);
            assert(prefix != 0U && prefix < payload.size());
            payload = payload.first(prefix);
        }
        assert(legacy.try_append_record(record->type, version, payload));
    }
    recorder::InMemoryMirroredTier0 tier0({8U, 1U, 1U, false});
    recorder::CompletionSlot completion;
    assert(tier0.try_append(legacy, completion) ==
           recorder::AppendStatus::Committed);
    CapturedBatch captured;
    assert(tier0.read(1U, captured.batch, captured.token) ==
           recorder::ReadStatus::Ok);
    assert(captured.token.authorization_envelope_version ==
           recorder::AuthorizationEnvelopeVersion::LegacyRangeV0);
    return captured;
}

[[nodiscard]] bool add_source_frame_authorizations(
    recorder::DurableBatch& destination,
    const CapturedBatch& source,
    const bool corrupt_first_session) noexcept {
    const auto record = source.batch.record(4U);
    if (!record || record->payload.size() < 2U) {
        return false;
    }
    const auto authorizations =
        source.token.external_effect_authorizations.values();
    const std::uint16_t count = get_u16(record->payload, 0U);
    if (count != authorizations.size()) {
        return false;
    }
    std::size_t offset = 2U;
    for (std::size_t index{}; index < count; ++index) {
        constexpr std::size_t kFixedThroughDispatchClass =
            4U + (4U * 8U) + 1U;
        if (record->payload.size() - offset <
            kFixedThroughDispatchClass + 1U) {
            return false;
        }
        offset += kFixedThroughDispatchClass;
        const std::size_t wire_size =
            std::to_integer<std::uint8_t>(record->payload[offset++]);
        if (wire_size > dataplane::kInlineWireIdentityBytes ||
            record->payload.size() - offset < wire_size + 2U) {
            return false;
        }
        offset += wire_size;
        const std::size_t frame_size = get_u16(record->payload, offset);
        offset += 2U;
        if (frame_size == 0U ||
            frame_size > fastpath::kMaximumEncodedFrameBytes ||
            record->payload.size() - offset < frame_size) {
            return false;
        }
        const recorder::ExternalEffectAuthorization& authorization =
            authorizations[index];
        const std::uint64_t session_id =
            corrupt_first_session && index == 0U
            ? authorization.venue_session_id + 1U
            : authorization.venue_session_id;
        if (session_id == 0U ||
            !destination.try_add_external_effect_authorization(
                authorization.venue_id,
                authorization.child_id,
                authorization.dispatch_attempt_id,
                session_id,
                authorization.venue_session_epoch,
                authorization.ownership_generation,
                authorization.dispatch_class,
                record->payload.subspan(offset, frame_size))) {
            return false;
        }
        offset += frame_size;
    }
    return true;
}

[[nodiscard]] CapturedBatch reauthorize_batch(
    const CapturedBatch& source,
    const recorder::AuthorizationEnvelopeVersion envelope,
    const bool corrupt_first_session = false,
    const CapturedBatch* const authorization_source = nullptr) {
    recorder::DurableBatch rebound(
        source.batch.lane(), source.batch.producer_sequence());
    rebound.set_leadership_epoch(source.batch.leadership_epoch());
    rebound.set_configuration_generation(
        source.batch.configuration_generation());
    assert(rebound.set_dispatch_authorization(
        source.batch.dispatch_authorization()));
    assert(rebound.set_authorization_envelope_version(envelope));
    if (envelope == recorder::AuthorizationEnvelopeVersion::ExactEffectsV1) {
        assert(add_source_frame_authorizations(
            rebound,
            authorization_source == nullptr ? source : *authorization_source,
            corrupt_first_session));
    }
    for (std::size_t index{}; index < source.batch.record_count(); ++index) {
        const auto record = source.batch.record(index);
        assert(record.has_value());
        assert(rebound.try_append_record(
            record->type, record->version, record->payload));
    }
    recorder::InMemoryMirroredTier0 tier0({8U, 1U, 1U, false});
    recorder::CompletionSlot completion;
    assert(tier0.try_append(rebound, completion) ==
           recorder::AppendStatus::Committed);
    CapturedBatch captured;
    assert(tier0.read(1U, captured.batch, captured.token) ==
           recorder::ReadStatus::Ok);
    return captured;
}

void finalize_checkpoint(std::vector<std::byte>& checkpoint) noexcept {
    constexpr std::size_t kFooterBytes = 36U;
    assert(checkpoint.size() >= kFooterBytes);
    const std::size_t hash_offset = checkpoint.size() - kFooterBytes;
    const std::size_t crc_offset = checkpoint.size() - 4U;
    TestSha256 sha;
    sha.update({checkpoint.data(), hash_offset});
    const recorder::HashChainDigest hash = sha.finish();
    std::copy(hash.begin(), hash.end(), checkpoint.begin() + hash_offset);
    put_u32(
        checkpoint,
        crc_offset,
        recorder::crc32c({checkpoint.data(), crc_offset}));
}

class PinnedCheckpointProvenanceVerifier final
    : public IRiskDispatchCheckpointProvenanceVerifier {
public:
    explicit PinnedCheckpointProvenanceVerifier(
        const std::span<const std::byte> checkpoint) noexcept
        : encoded_size_(checkpoint.size()),
          crc32c_(get_u32(checkpoint, checkpoint.size() - 4U)) {
        constexpr std::size_t kCheckpointHashBytes = 32U;
        constexpr std::size_t kCheckpointFooterBytes = 36U;
        assert(checkpoint.size() >= kCheckpointFooterBytes);
        std::copy_n(
            checkpoint.end() - kCheckpointFooterBytes,
            kCheckpointHashBytes,
            checkpoint_hash_.begin());
    }

    [[nodiscard]] bool verify_checkpoint(
        const std::span<const std::byte> encoded_checkpoint,
        const RiskDispatchCheckpointProvenanceContext& context)
        const noexcept override {
        return encoded_checkpoint.size() == encoded_size_ &&
               get_u32(encoded_checkpoint, encoded_checkpoint.size() - 4U) ==
                   crc32c_ &&
               context.checkpoint_hash == checkpoint_hash_ &&
               context.expected_leadership_epoch == kLeadershipEpoch &&
               context.expected_configuration_generation ==
                   kConfigurationGeneration &&
               context.expected_domain ==
                   recorder::DurabilityDomain::
                       VolatileSoftwareReference;
    }

private:
    std::size_t encoded_size_{};
    std::uint32_t crc32c_{};
    recorder::HashChainDigest checkpoint_hash_{};
};

[[nodiscard]] std::vector<std::byte> make_legacy_checkpoint_v1(
    const std::span<const std::byte> current,
    const bool force_cancel_pending = false) {
    constexpr std::size_t kHeaderBytes = 174U;
    constexpr std::size_t kFooterBytes = 36U;
    constexpr std::size_t kParentBytes = 32U;
    constexpr std::size_t kLegacyChildBytes = 64U;
    assert(current.size() >= kHeaderBytes + kFooterBytes);
    assert(get_u16(current, 4U) ==
           kRiskDispatchRecoveryCheckpointVersion);
    const std::size_t current_hash_offset = current.size() - kFooterBytes;
    {
        TestSha256 current_sha;
        current_sha.update(current.first(current_hash_offset));
        const recorder::HashChainDigest expected = current_sha.finish();
        assert(std::equal(
            expected.begin(),
            expected.end(),
            current.begin() + current_hash_offset));
        assert(recorder::crc32c(current.first(current.size() - 4U)) ==
               get_u32(current, current.size() - 4U));
    }
    const std::uint32_t parent_count = get_u32(current, 146U);
    std::vector<std::byte> legacy(current.size());
    std::copy_n(current.begin(), kHeaderBytes, legacy.begin());
    put_u16(
        legacy,
        4U,
        kRiskDispatchRecoveryMinimumCheckpointVersion);
    std::size_t source_offset = kHeaderBytes;
    std::size_t destination_offset = kHeaderBytes;
    for (std::size_t parent_index{}; parent_index < parent_count;
         ++parent_index) {
        assert(current_hash_offset - source_offset >= kParentBytes);
        const std::uint16_t child_count =
            get_u16(current, source_offset + 14U);
        assert(child_count != 0U);
        std::copy_n(
            current.begin() + source_offset,
            kParentBytes,
            legacy.begin() + destination_offset);
        source_offset += kParentBytes;
        destination_offset += kParentBytes;
        for (std::size_t child_index{}; child_index < child_count;
             ++child_index) {
            assert(current_hash_offset - source_offset >= 130U);
            const std::uint64_t child = get_u64(current, source_offset);
            const std::uint64_t attempt = get_u64(current, source_offset + 8U);
            const std::uint64_t last = get_u64(current, source_offset + 16U);
            const std::uint32_t venue = get_u32(current, source_offset + 24U);
            const std::uint64_t session = get_u64(current, source_offset + 28U);
            const std::uint64_t session_epoch =
                get_u64(current, source_offset + 36U);
            const std::uint64_t ownership =
                get_u64(current, source_offset + 44U);
            const std::size_t wire_size =
                std::to_integer<std::uint8_t>(current[source_offset + 52U]);
            assert(child != 0U && attempt != 0U && last == attempt &&
                   venue != 0U && session != 0U && session_epoch != 0U &&
                   ownership != 0U && wire_size != 0U &&
                   wire_size <= dataplane::kInlineWireIdentityBytes);
            const std::size_t accounting_offset =
                source_offset + 53U + wire_size;
            constexpr std::size_t kAccountingAndFlagsBytes = 44U;
            constexpr std::size_t kPendingTupleBytes = 32U;
            assert(current_hash_offset - accounting_offset >=
                   kAccountingAndFlagsBytes + kPendingTupleBytes);
            assert(current[accounting_offset +
                           kAccountingAndFlagsBytes - 1U] == std::byte{});
            put_u64(legacy, destination_offset, child);
            put_u64(legacy, destination_offset + 8U, attempt);
            put_u32(legacy, destination_offset + 16U, venue);
            std::copy_n(
                current.begin() + accounting_offset,
                kAccountingAndFlagsBytes,
                legacy.begin() + destination_offset + 20U);
            if (force_cancel_pending && parent_index == 0U &&
                child_index == 0U) {
                legacy[destination_offset + kLegacyChildBytes - 1U] =
                    std::byte{1U};
            }
            destination_offset += kLegacyChildBytes;
            source_offset = accounting_offset +
                kAccountingAndFlagsBytes + kPendingTupleBytes;
        }
    }
    assert(source_offset <= current_hash_offset);
    const std::size_t tail_size = current_hash_offset - source_offset;
    std::copy_n(
        current.begin() + source_offset,
        tail_size,
        legacy.begin() + destination_offset);
    destination_offset += tail_size;
    legacy.resize(destination_offset + kFooterBytes);
    finalize_checkpoint(legacy);
    {
        const std::size_t legacy_hash_offset = legacy.size() - kFooterBytes;
        TestSha256 legacy_sha;
        legacy_sha.update(
            {legacy.data(), legacy_hash_offset});
        const recorder::HashChainDigest expected = legacy_sha.finish();
        assert(std::equal(
            expected.begin(),
            expected.end(),
            legacy.begin() + legacy_hash_offset));
        assert(recorder::crc32c(
                   {legacy.data(), legacy.size() - 4U}) ==
               get_u32(legacy, legacy.size() - 4U));
    }
    return legacy;
}

void test_adaptive_overlay_record_replay() {
    const CapturedBatch adaptive = produce(false, true);
    RecoveryFixture fixture;
    const auto result = fixture.ledger.replay_dispatch_batch(
        adaptive.token,
        adaptive.batch,
        RecoveryCommitPhase::DurableOnly);
    assert(result.status == RiskDispatchReplayStatus::Applied);
    assert_recovered(fixture);

    const CapturedBatch excluded = produce(false, true, true);
    RecoveryFixture excluded_fixture;
    const auto excluded_result =
        excluded_fixture.ledger.replay_dispatch_batch(
            excluded.token,
            excluded.batch,
            RecoveryCommitPhase::DurableOnly);
    assert(excluded_result.status == RiskDispatchReplayStatus::Applied);
    assert(excluded_result.child_count == 1U);
    assert(excluded_fixture.risk_kernel.client_counters(kClient)
               .open_quantity == Quantity{6U});
    assert(excluded_fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{6U});
}

void test_legacy_record_migration_and_effect_binding(
    const CapturedBatch& captured) {
    const CapturedBatch legacy_v1 = make_legacy_batch(captured, 1U);
    const std::array phases{
        RecoveryCommitPhase::DurableOnly,
        RecoveryCommitPhase::RiskCommitted,
        RecoveryCommitPhase::RiskAndRouteCommitted};
    for (const RecoveryCommitPhase phase : phases) {
        RecoveryFixture fixture;
        std::size_t allocations{};
        RiskDispatchReplayResult result;
        {
            allocation_probe::Scope scope;
            result = fixture.ledger.replay_dispatch_batch(
                legacy_v1.token, legacy_v1.batch, phase);
            allocations = scope.allocations();
        }
        assert(allocations == 0U);
        assert(result.status == RiskDispatchReplayStatus::Applied);
        // v1 durably reserved all 12 units but did not describe the release.
        // Migration derives the authenticated IOC remainder from 0x1204 and
        // repairs the historical two-unit phantom reservation.
        assert_recovered(fixture);
    }

    {
        RecoveryFixture fixture;
        assert(fixture.ledger.replay_dispatch_batch(
                   legacy_v1.token,
                   legacy_v1.batch,
                   RecoveryCommitPhase::DurableOnly)
                   .status == RiskDispatchReplayStatus::Applied);
        const auto authorization = legacy_v1.token.dispatch_authorization;
        assert(fixture.ledger.apply_recovered_fill(
                   ParentOrderId{1U},
                   ChildOrderId{authorization.first_child_id},
                   authorization.first_attempt_id,
                   Quantity{4U})
                   .status == RiskDispatchAccountingStatus::Applied);
        assert(fixture.ledger.apply_recovered_fill(
                   ParentOrderId{1U},
                   ChildOrderId{authorization.first_child_id + 1U},
                   authorization.first_attempt_id + 1U,
                   Quantity{6U})
                   .status == RiskDispatchAccountingStatus::Applied);
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
        assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
               Quantity{});
        assert(fixture.route_kernel.instrument_counters(kInstrument)
                   .gross_outstanding == Quantity{});
    }

    const CapturedBatch adaptive = produce(false, true);
    const CapturedBatch legacy_v2 = make_legacy_batch(adaptive, 2U);
    {
        RecoveryFixture fixture;
        assert(fixture.ledger.replay_dispatch_batch(
                   legacy_v2.token,
                   legacy_v2.batch,
                   RecoveryCommitPhase::DurableOnly)
                   .status == RiskDispatchReplayStatus::Applied);
        assert_recovered(fixture);
    }

    const CapturedBatch wrong_session = reauthorize_batch(
        captured,
        recorder::AuthorizationEnvelopeVersion::ExactEffectsV1,
        true);
    {
        RecoveryFixture fixture;
        assert(fixture.ledger.replay_dispatch_batch(
                   wrong_session.token,
                   wrong_session.batch,
                   RecoveryCommitPhase::DurableOnly)
                   .status == RiskDispatchReplayStatus::TokenMismatch);
        assert(fixture.ledger.fail_stopped());
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
    }

    const CapturedBatch legacy_envelope_v3 = reauthorize_batch(
        captured, recorder::AuthorizationEnvelopeVersion::LegacyRangeV0);
    {
        RecoveryFixture fixture;
        assert(fixture.ledger.replay_dispatch_batch(
                   legacy_envelope_v3.token,
                   legacy_envelope_v3.batch,
                   RecoveryCommitPhase::DurableOnly)
                   .status == RiskDispatchReplayStatus::TokenMismatch);
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
    }

    const CapturedBatch exact_envelope_v1 = reauthorize_batch(
        legacy_v1,
        recorder::AuthorizationEnvelopeVersion::ExactEffectsV1,
        false,
        &captured);
    {
        RecoveryFixture fixture;
        assert(fixture.ledger.replay_dispatch_batch(
                   exact_envelope_v1.token,
                   exact_envelope_v1.batch,
                   RecoveryCommitPhase::DurableOnly)
                   .status == RiskDispatchReplayStatus::TokenMismatch);
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
    }
}

void test_duplicate_conflict(
    const CapturedBatch& captured,
    const CapturedBatch& conflict) {
    assert(captured.token.batch_id == conflict.token.batch_id);
    assert(captured.token.batch_hash != conflict.token.batch_hash);
    RecoveryFixture fixture;
    assert(fixture.ledger.replay_dispatch_batch(
               captured.token,
               captured.batch,
               RecoveryCommitPhase::DurableOnly)
               .applied());
    const auto result = fixture.ledger.replay_dispatch_batch(
        conflict.token,
        conflict.batch,
        RecoveryCommitPhase::DurableOnly);
    assert(result.status == RiskDispatchReplayStatus::DuplicateConflict);
    assert(fixture.ledger.fail_stopped());
    assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{10U});
    assert(fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{10U});
}

void assert_recovered(const RecoveryFixture& fixture) {
    assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{10U});
    assert(fixture.risk_kernel.client_counters(kClient).open_notional ==
           Notional{1'010U});
    assert(fixture.risk_kernel.active_parent(ParentOrderId{1U})
               .remaining_quantity == Quantity{10U});
    assert(fixture.route_kernel.active_parent_count() == 1U);
    assert(fixture.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{10U});
    assert(fixture.ledger.metadata().unreconciled_children == 2U);
}

void test_all_crash_phases_and_exactly_once(const CapturedBatch& captured) {
    const std::array phases{
        RecoveryCommitPhase::DurableOnly,
        RecoveryCommitPhase::RiskCommitted,
        RecoveryCommitPhase::RiskAndRemainderReleased,
        RecoveryCommitPhase::RiskAndRouteCommitted};
    for (const RecoveryCommitPhase phase : phases) {
        RecoveryFixture fixture;
        std::size_t allocations{};
        RiskDispatchReplayResult replayed;
        {
            allocation_probe::Scope scope;
            replayed = fixture.ledger.replay_dispatch_batch(
                captured.token, captured.batch, phase);
            allocations = scope.allocations();
        }
        assert(allocations == 0U);
        assert(replayed.status == RiskDispatchReplayStatus::Applied);
        assert(replayed.observed_crash_phase == phase);
        assert(replayed.child_count == 2U);
        assert_recovered(fixture);
        const auto before = fixture.ledger.metadata();
        const RiskDispatchReplayResult duplicate =
            fixture.ledger.replay_dispatch_batch(
                captured.token, captured.batch, phase);
        assert(duplicate.status == RiskDispatchReplayStatus::AlreadyApplied);
        assert(fixture.ledger.metadata().active_parents ==
               before.active_parents);
        assert(fixture.risk_kernel.client_counters(kClient).open_quantity ==
               Quantity{10U});
    }
}

void test_integrity_and_missing_record_fail_closed(
    const CapturedBatch& captured) {
    {
        RecoveryFixture fixture;
        recorder::DurableToken tampered = captured.token;
        tampered.payload_crc32c ^= 1U;
        const auto result = fixture.ledger.replay_dispatch_batch(
            tampered, captured.batch, RecoveryCommitPhase::DurableOnly);
        assert(result.status == RiskDispatchReplayStatus::IntegrityFailure);
        assert(fixture.ledger.fail_stopped());
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
    }
    {
        recorder::DurableBatch incomplete(
            recorder::DurabilityLane::Normal,
            captured.batch.producer_sequence());
        incomplete.set_leadership_epoch(captured.batch.leadership_epoch());
        incomplete.set_configuration_generation(
            captured.batch.configuration_generation());
        assert(incomplete.set_dispatch_authorization(
            captured.batch.dispatch_authorization()));
        assert(incomplete.set_authorization_envelope_version(
            recorder::AuthorizationEnvelopeVersion::LegacyRangeV0));
        for (std::size_t index{}; index < 4U; ++index) {
            const auto record = captured.batch.record(index);
            assert(record.has_value());
            assert(incomplete.try_append_record(
                record->type, record->version, record->payload));
        }
        recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, false});
        recorder::CompletionSlot completion;
        assert(recorder.try_append(incomplete, completion) ==
               recorder::AppendStatus::Committed);
        const auto token = completion.token();
        assert(token.has_value());
        RecoveryFixture fixture;
        const auto result = fixture.ledger.replay_dispatch_batch(
            *token, incomplete, RecoveryCommitPhase::DurableOnly);
        assert(result.status == RiskDispatchReplayStatus::InvalidRecordSet);
        assert(fixture.ledger.fail_stopped());
    }
    {
        recorder::DurableBatch semantically_corrupt(
            recorder::DurabilityLane::Normal,
            captured.batch.producer_sequence());
        semantically_corrupt.set_leadership_epoch(
            captured.batch.leadership_epoch());
        semantically_corrupt.set_configuration_generation(
            captured.batch.configuration_generation());
        assert(semantically_corrupt.set_dispatch_authorization(
            captured.batch.dispatch_authorization()));
        assert(semantically_corrupt.set_authorization_envelope_version(
            recorder::AuthorizationEnvelopeVersion::LegacyRangeV0));
        for (std::size_t index{}; index < 5U; ++index) {
            const auto record = captured.batch.record(index);
            assert(record.has_value());
            if (index != 1U) {
                assert(semantically_corrupt.try_append_record(
                    record->type, record->version, record->payload));
                continue;
            }
            std::array<std::byte, recorder::kMaximumDurableBatchBytes>
                altered{};
            assert(!record->payload.empty());
            std::copy(
                record->payload.begin(), record->payload.end(), altered.begin());
            altered[record->payload.size() - 1U] ^= std::byte{1U};
            assert(semantically_corrupt.try_append_record(
                record->type,
                record->version,
                std::span<const std::byte>{
                    altered.data(), record->payload.size()}));
        }
        recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, false});
        recorder::CompletionSlot completion;
        assert(recorder.try_append(semantically_corrupt, completion) ==
               recorder::AppendStatus::Committed);
        const auto token = completion.token();
        assert(token.has_value());
        RecoveryFixture fixture;
        const auto result = fixture.ledger.replay_dispatch_batch(
            *token,
            semantically_corrupt,
            RecoveryCommitPhase::DurableOnly);
        assert(result.status ==
               RiskDispatchReplayStatus::CanonicalDecodeFailed);
        assert(fixture.ledger.fail_stopped());
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
    }
    {
        RecoveryFixture fixture;
        const auto result = fixture.ledger.replay_dispatch_batch(
            captured.token,
            captured.batch,
            static_cast<RecoveryCommitPhase>(255U));
        assert(result.status == RiskDispatchReplayStatus::InvalidBatch);
        assert(fixture.ledger.fail_stopped());
        assert(fixture.risk_kernel.metadata().active_parents == 0U);
        assert(fixture.route_kernel.active_parent_count() == 0U);
    }
}

void test_checkpoint_second_crash_and_accounting(
    const CapturedBatch& captured) {
    RecoveryFixture first;
    assert(first.ledger.replay_dispatch_batch(
               captured.token,
               captured.batch,
               RecoveryCommitPhase::RiskAndRouteCommitted)
               .applied());
    const auto authorization = captured.token.dispatch_authorization;
    assert(first.ledger.apply_recovered_fill(
               ParentOrderId{1U},
               ChildOrderId{authorization.first_child_id},
               authorization.first_attempt_id,
               Quantity{2U})
               .status == RiskDispatchAccountingStatus::Applied);
    assert(first.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{8U});
    constexpr std::uint64_t cancel_attempt_id = 1'000U;
    constexpr std::uint64_t cancel_request_id = 700U;
    constexpr std::uint64_t cancel_client_sequence = 44U;
    constexpr std::uint64_t cancel_session_epoch = 3U;
    assert(first.ledger.note_recovered_cancel(
               ParentOrderId{1U},
               ChildOrderId{authorization.first_child_id + 1U},
               authorization.first_attempt_id + 1U,
               cancel_attempt_id,
               cancel_request_id,
               cancel_client_sequence,
               cancel_session_epoch)
               .status == RiskDispatchAccountingStatus::Applied);

    const std::size_t required = first.ledger.checkpoint_size_required();
    assert(required != 0U);
    std::vector<std::byte> checkpoint(required);
    std::size_t allocations{};
    RiskDispatchCheckpointEncodeResult encoded;
    {
        allocation_probe::Scope scope;
        encoded = first.ledger.encode_checkpoint(checkpoint);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(encoded.status == RiskDispatchCheckpointEncodeStatus::Encoded);
    assert(encoded.encoded_size == checkpoint.size());

    RecoveryFixture second;
    RiskDispatchCheckpointRestoreResult restored;
    {
        allocation_probe::Scope scope;
        restored = second.ledger.restore_checkpoint(checkpoint);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(restored.status == RiskDispatchCheckpointRestoreStatus::Restored);
    assert(restored.restored_parents == 1U);
    assert(second.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{8U});
    assert(second.route_kernel.instrument_counters(kInstrument)
               .gross_outstanding == Quantity{8U});
    assert(second.ledger.metadata().replayed_through_batch_id ==
           captured.token.batch_id);
    const ActiveChildLookupResult pending =
        second.route_kernel.lookup_active_child(
            ChildOrderId{authorization.first_child_id + 1U});
    assert(pending.status == ActiveChildLookupStatus::Found);
    assert(pending.snapshot.state == ActiveChildOwnershipState::CancelPending);
    assert(pending.snapshot.last_dispatch_attempt_id == cancel_attempt_id);
    assert(pending.snapshot.pending_cancel_attempt_id == cancel_attempt_id);
    assert(pending.snapshot.pending_client_request_id == cancel_request_id);
    assert(pending.snapshot.pending_client_sequence == cancel_client_sequence);
    assert(pending.snapshot.pending_client_session_epoch ==
           cancel_session_epoch);
    assert(second.ledger.note_recovered_cancel(
               ParentOrderId{1U},
               ChildOrderId{authorization.first_child_id + 1U},
               authorization.first_attempt_id + 1U,
               cancel_attempt_id,
               cancel_request_id,
               cancel_client_sequence,
               cancel_session_epoch)
               .status == RiskDispatchAccountingStatus::AlreadyApplied);
    assert(second.ledger.replay_dispatch_batch(
               captured.token,
               captured.batch,
               RecoveryCommitPhase::DurableOnly)
               .status == RiskDispatchReplayStatus::AlreadyApplied);

    RiskDispatchAccountingResult filled;
    RiskDispatchAccountingResult reconciled;
    {
        allocation_probe::Scope scope;
        filled = second.ledger.apply_recovered_fill(
            ParentOrderId{1U},
            ChildOrderId{authorization.first_child_id},
            authorization.first_attempt_id,
            Quantity{2U});
        reconciled = second.ledger.reconcile_recovered_child(
            ParentOrderId{1U},
            ChildOrderId{authorization.first_child_id + 1U},
            authorization.first_attempt_id + 1U,
            RecoveredChildDisposition::Working);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(filled.status == RiskDispatchAccountingStatus::Applied);
    assert(reconciled.status == RiskDispatchAccountingStatus::Applied);
    assert(second.ledger.metadata().unreconciled_children == 0U);
    assert(second.ledger.complete_reconciliation());
    assert(!second.risk_kernel.recovery_only());
    assert(!second.route_kernel.recovery_only());
    assert(second.route_kernel.kill_switch());
    assert(!second.route_kernel.routing_enabled());
    assert(second.risk_kernel.client_counters(kClient).open_quantity ==
           Quantity{6U});
    assert(second.route_kernel.release_child(ParentOrderId{1U}, kVenueTwo) ==
           ReleaseStatus::Released);
    assert(second.risk_kernel.fill(ParentOrderId{1U}, Quantity{6U}) ==
           risk::RiskAccountingStatus::Applied);
    assert(second.risk_kernel.metadata().active_parents == 0U);
    assert(second.route_kernel.active_parent_count() == 0U);

    std::vector<std::byte> corrupt = checkpoint;
    corrupt[20U] ^= std::byte{1U};
    RecoveryFixture corrupt_target;
    assert(corrupt_target.ledger.restore_checkpoint(corrupt).status ==
           RiskDispatchCheckpointRestoreStatus::IntegrityFailure);
    assert(corrupt_target.ledger.fail_stopped());
}

void test_checkpoint_external_provenance_gate(
    const CapturedBatch& captured) {
    RecoveryFixture source;
    assert(source.ledger.replay_dispatch_batch(
               captured.token,
               captured.batch,
               RecoveryCommitPhase::DurableOnly)
               .status == RiskDispatchReplayStatus::Applied);
    std::vector<std::byte> checkpoint(
        source.ledger.checkpoint_size_required());
    assert(source.ledger.encode_checkpoint(checkpoint).status ==
           RiskDispatchCheckpointEncodeStatus::Encoded);

    const PinnedCheckpointProvenanceVerifier verifier(checkpoint);
    RiskDispatchRecoveryConfiguration verified_configuration =
        recovery_configuration();
    verified_configuration.checkpoint_trust_policy =
        RiskDispatchCheckpointTrustPolicy::RequireExternalProvenance;
    verified_configuration.checkpoint_provenance_verifier = &verifier;
    RecoveryFixture verified(verified_configuration);
    std::size_t allocations{};
    RiskDispatchCheckpointRestoreResult result;
    {
        allocation_probe::Scope scope;
        result = verified.ledger.restore_checkpoint(checkpoint);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(result.status == RiskDispatchCheckpointRestoreStatus::Restored);

    RiskDispatchRecoveryConfiguration missing_configuration =
        verified_configuration;
    missing_configuration.checkpoint_provenance_verifier = nullptr;
    RecoveryFixture missing(missing_configuration);
    assert(missing.ledger.restore_checkpoint(checkpoint).status ==
           RiskDispatchCheckpointRestoreStatus::ProvenanceRequired);
    assert(missing.ledger.fail_stopped());
    assert(missing.risk_kernel.metadata().active_parents == 0U);
    assert(missing.route_kernel.active_parent_count() == 0U);

    // An attacker can recompute the unkeyed inner SHA-256 and CRC after
    // changing a valid field. The externally pinned checkpoint root remains
    // different, so the production restore capability rejects before import.
    constexpr std::size_t kRiskKillSwitchOffset = 97U;
    std::vector<std::byte> forged = checkpoint;
    assert(forged[kRiskKillSwitchOffset] == std::byte{});
    forged[kRiskKillSwitchOffset] = std::byte{1U};
    finalize_checkpoint(forged);
    RecoveryFixture rejected(verified_configuration);
    {
        allocation_probe::Scope scope;
        result = rejected.ledger.restore_checkpoint(forged);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(result.status ==
           RiskDispatchCheckpointRestoreStatus::ProvenanceRejected);
    assert(rejected.ledger.fail_stopped());
    assert(rejected.risk_kernel.metadata().active_parents == 0U);
    assert(rejected.route_kernel.active_parent_count() == 0U);
}

void test_legacy_checkpoint_v1_forward_migration(
    const CapturedBatch& captured) {
    const CapturedBatch legacy_batch = make_legacy_batch(captured, 1U);
    RecoveryFixture source;
    assert(source.ledger.replay_dispatch_batch(
               legacy_batch.token,
               legacy_batch.batch,
               RecoveryCommitPhase::DurableOnly)
               .status == RiskDispatchReplayStatus::Applied);
    std::vector<std::byte> current_checkpoint(
        source.ledger.checkpoint_size_required());
    assert(source.ledger.encode_checkpoint(current_checkpoint).status ==
           RiskDispatchCheckpointEncodeStatus::Encoded);
    const std::vector<std::byte> legacy_checkpoint =
        make_legacy_checkpoint_v1(current_checkpoint);

    RecoveryFixture first_restart;
    RiskDispatchCheckpointRestoreResult restored;
    std::size_t allocations{};
    {
        allocation_probe::Scope scope;
        restored = first_restart.ledger.restore_checkpoint(legacy_checkpoint);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(restored.status == RiskDispatchCheckpointRestoreStatus::Restored);
    assert_recovered(first_restart);
    const auto authorization = legacy_batch.token.dispatch_authorization;
    const ActiveChildLookupResult migrated =
        first_restart.route_kernel.lookup_active_child(
            ChildOrderId{authorization.first_child_id});
    assert(migrated.status == ActiveChildLookupStatus::Found);
    assert(migrated.snapshot.venue_session_id == 1U);
    assert(migrated.snapshot.venue_session_epoch == 1U);
    assert(migrated.snapshot.ownership_generation != 0U);
    assert(migrated.snapshot.last_dispatch_attempt_id ==
           authorization.first_attempt_id);
    assert(migrated.snapshot.wire_identity.size ==
           dataplane::kInlineWireIdentityBytes);
    assert(migrated.snapshot.wire_identity.bytes[0U] == std::byte{'S'});
    assert(migrated.snapshot.wire_identity.bytes[1U] == std::byte{'R'});
    assert(migrated.snapshot.wire_identity.bytes[2U] == std::byte{'X'});
    assert(migrated.snapshot.wire_identity.bytes[3U] == std::byte{1U});

    // A checkpoint emitted after the v1 restore is always current v2. Restore
    // it again to model a second crash after the forward migration.
    std::vector<std::byte> migrated_checkpoint(
        first_restart.ledger.checkpoint_size_required());
    assert(first_restart.ledger.encode_checkpoint(migrated_checkpoint).status ==
           RiskDispatchCheckpointEncodeStatus::Encoded);
    assert(get_u16(migrated_checkpoint, 4U) ==
           kRiskDispatchRecoveryCheckpointVersion);
    RecoveryFixture second_restart;
    {
        allocation_probe::Scope scope;
        restored = second_restart.ledger.restore_checkpoint(migrated_checkpoint);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(restored.status == RiskDispatchCheckpointRestoreStatus::Restored);
    assert(second_restart.ledger.apply_recovered_fill(
               ParentOrderId{1U},
               ChildOrderId{authorization.first_child_id},
               authorization.first_attempt_id,
               Quantity{4U})
               .status == RiskDispatchAccountingStatus::Applied);
    assert(second_restart.ledger.apply_recovered_fill(
               ParentOrderId{1U},
               ChildOrderId{authorization.first_child_id + 1U},
               authorization.first_attempt_id + 1U,
               Quantity{6U})
               .status == RiskDispatchAccountingStatus::Applied);
    assert(second_restart.risk_kernel.metadata().active_parents == 0U);
    assert(second_restart.route_kernel.active_parent_count() == 0U);

    // A v1 row cannot faithfully identify an in-flight cancel retry. Reject
    // it instead of inventing the missing client/session/attempt tuple.
    const std::vector<std::byte> ambiguous_pending =
        make_legacy_checkpoint_v1(current_checkpoint, true);
    RecoveryFixture rejected;
    assert(rejected.ledger.restore_checkpoint(ambiguous_pending).status ==
           RiskDispatchCheckpointRestoreStatus::InvalidEncoding);
    assert(rejected.ledger.fail_stopped());
    assert(rejected.risk_kernel.metadata().active_parents == 0U);
    assert(rejected.route_kernel.active_parent_count() == 0U);
}

static_assert(noexcept(std::declval<RiskDispatchRecoveryLedger&>()
    .replay_dispatch_batch(
        std::declval<const recorder::DurableToken&>(),
        std::declval<const recorder::DurableBatch&>(),
        RecoveryCommitPhase::DurableOnly)));
static_assert(noexcept(std::declval<RiskDispatchRecoveryLedger&>()
    .restore_checkpoint(std::declval<std::span<const std::byte>>())));
static_assert(noexcept(std::declval<const RiskDispatchRecoveryLedger&>()
    .encode_checkpoint(std::declval<std::span<std::byte>>())));
static_assert(noexcept(std::declval<RiskDispatchRecoveryLedger&>()
    .note_recovered_cancel(
        ParentOrderId{1U},
        ChildOrderId{1U},
        1U,
        2U,
        1U,
        1U,
        1U)));
static_assert(
    RiskDispatchRecoveryConfiguration{}.required_domain ==
    recorder::DurabilityDomain::HardwarePersistent);
static_assert(
    RiskDispatchRecoveryConfiguration{}.checkpoint_trust_policy ==
    RiskDispatchCheckpointTrustPolicy::RequireExternalProvenance);
static_assert(noexcept(std::declval<
    const IRiskDispatchCheckpointProvenanceVerifier&>()
    .verify_checkpoint(
        std::declval<std::span<const std::byte>>(),
        std::declval<
            const RiskDispatchCheckpointProvenanceContext&>())));
static_assert(kRiskDispatchRecoveryMinimumCheckpointVersion == 1U);
static_assert(kRiskDispatchRecoveryCheckpointVersion == 2U);

}  // namespace

int main() {
    const CapturedBatch captured = produce();
    const CapturedBatch conflict = produce(true);
    test_all_crash_phases_and_exactly_once(captured);
    test_adaptive_overlay_record_replay();
    test_legacy_record_migration_and_effect_binding(captured);
    test_duplicate_conflict(captured, conflict);
    test_integrity_and_missing_record_fail_closed(captured);
    test_checkpoint_second_crash_and_accounting(captured);
    test_checkpoint_external_provenance_gate(captured);
    test_legacy_checkpoint_v1_forward_migration(captured);
    return 0;
}
