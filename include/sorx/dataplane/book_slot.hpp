#pragma once

#include "sorx/book_view.hpp"
#include "sorx/policy/adaptive_policy.hpp"
#include "sorx/routing_policy.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace sorx::dataplane {

inline constexpr std::uint16_t kAdaptivePolicyUnranked =
    std::numeric_limits<std::uint16_t>::max();

// Order-specific immutable policy evidence attached to a caller-owned view
// after AdaptivePolicyKernel evaluation. An excluded candidate intentionally
// has no RankedCandidate evidence; its exact hard reason and fact pin remain.
struct AdaptivePolicyOverlay final {
    policy::PolicyVersionPin versions{};
    policy::PolicyOrder order{};
    policy::VenueFactPin fact_pin{};
    policy::RankedCandidate evidence{};
    policy::PolicyExclusionReason exclusion_reason{
        policy::PolicyExclusionReason::None};
    std::uint16_t rank{kAdaptivePolicyUnranked};
    bool applied{false};
    bool eligible{false};

    [[nodiscard]] constexpr bool valid_shape() const noexcept {
        if (!applied) {
            return !eligible && rank == kAdaptivePolicyUnranked;
        }
        if (!versions.valid() || !order.valid() ||
            !static_cast<bool>(fact_pin.venue)) {
            return false;
        }
        if (eligible) {
            return rank != kAdaptivePolicyUnranked &&
                   exclusion_reason == policy::PolicyExclusionReason::None &&
                   evidence.venue == fact_pin.venue &&
                   evidence.pin == fact_pin &&
                   evidence.executable_price.units > 0;
        }
        return rank == kAdaptivePolicyUnranked &&
               exclusion_reason != policy::PolicyExclusionReason::None &&
               !static_cast<bool>(evidence.venue);
    }

    constexpr auto operator<=>(const AdaptivePolicyOverlay&) const noexcept = default;
};

// The complete immutable input consumed by one routing decision for one
// venue/instrument adjacency. The owning market-data/session thread builds a
// value locally and publishes it as one unit.
struct BookSlotView final {
    VenueId venue{};
    InstrumentId instrument{};
    std::uint64_t adjacency_generation{};
    md::BookView book{};
    MarketDataState market_data_state{MarketDataState::Snapshotting};
    OrderEntryState order_entry_state{OrderEntryState::Disconnected};
    EgressState egress_state{EgressState::Ready};
    InstrumentState instrument_state{InstrumentState::Unknown};
    std::uint64_t feed_epoch{};
    std::uint64_t source_health_generation{};
    std::uint64_t monotonic_receive_time_ns{};
    std::uint64_t monotonic_publication_time_ns{};
    std::uint64_t maximum_quote_age_ns{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t normal_tx_credits{};
    std::uint32_t cancel_reserved_tx_credits{};
    std::uint32_t throttle_credits{};
    bool has_book{};
    bool source_healthy{};
    bool conversion_compatible{true};
    bool risk_allowed{true};
    AdaptivePolicyOverlay adaptive_policy{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        const bool valid_market_data_state =
            market_data_state == MarketDataState::Snapshotting ||
            market_data_state == MarketDataState::Stitching ||
            market_data_state == MarketDataState::Live ||
            market_data_state == MarketDataState::Recovering ||
            market_data_state == MarketDataState::Failed;
        const bool valid_order_entry_state =
            order_entry_state == OrderEntryState::Disconnected ||
            order_entry_state == OrderEntryState::Connecting ||
            order_entry_state == OrderEntryState::Ready ||
            order_entry_state == OrderEntryState::Degraded;
        const bool valid_egress_state =
            egress_state == EgressState::Ready ||
            egress_state == EgressState::Congested;
        const bool valid_instrument_state =
            instrument_state == InstrumentState::Unknown ||
            instrument_state == InstrumentState::Trading ||
            instrument_state == InstrumentState::Halted ||
            instrument_state == InstrumentState::Closed;
        if (!venue || !instrument || adjacency_generation == 0U ||
            source_health_generation == 0U ||
            maximum_quote_age_ns == 0U || tx_credit_generation == 0U ||
            throttle_generation == 0U ||
            monotonic_publication_time_ns < monotonic_receive_time_ns ||
            !valid_market_data_state || !valid_order_entry_state ||
            !valid_egress_state || !valid_instrument_state) {
            return false;
        }
        if (!has_book) {
            return !source_healthy;
        }
        return feed_epoch != 0U && book.valid() && book.venue == venue &&
               book.instrument == instrument &&
               book.feed_session_epoch == feed_epoch;
    }

    [[nodiscard]] constexpr bool fresh_at(
        const std::uint64_t monotonic_time_ns) const noexcept {
        return has_book && source_healthy &&
               monotonic_time_ns >= monotonic_publication_time_ns &&
               monotonic_time_ns >= monotonic_receive_time_ns &&
               monotonic_time_ns - monotonic_receive_time_ns <=
                   maximum_quote_age_ns;
    }
};

static_assert(std::is_trivially_copyable_v<BookSlotView>);
static_assert(std::is_standard_layout_v<BookSlotView>);

enum class AdaptiveOverlayApplyStatus : std::uint8_t {
    Applied = 0U,
    InvalidDecision,
    CapacityMismatch,
    InvalidView,
    DuplicateVenue,
    AssessmentMissing,
    AssessmentDuplicate,
};

struct AdaptiveOverlayApplyResult final {
    AdaptiveOverlayApplyStatus status{AdaptiveOverlayApplyStatus::InvalidDecision};
    std::uint16_t applied_count{0U};

    [[nodiscard]] constexpr bool applied() const noexcept {
        return status == AdaptiveOverlayApplyStatus::Applied;
    }
};

// Validates the complete mapping before mutating any view. The decision and
// views are caller-owned; this helper allocates nothing and leaves every view
// unchanged on failure.
[[nodiscard]] inline AdaptiveOverlayApplyResult apply_adaptive_policy_decision(
    const policy::PolicyDecision& decision,
    const std::span<BookSlotView> views) noexcept {
    if (!decision.versions.valid() || !decision.order.valid() ||
        decision.intent != decision.order.intent ||
        decision.ranked_count > decision.ranked.size() ||
        decision.exclusion_count > decision.exclusions.size() ||
        (decision.status != policy::PolicyEvaluationStatus::Ranked &&
         decision.status != policy::PolicyEvaluationStatus::NoEligibleCandidate) ||
        (decision.status == policy::PolicyEvaluationStatus::Ranked &&
         decision.ranked_count == 0U) ||
        (decision.status == policy::PolicyEvaluationStatus::NoEligibleCandidate &&
         decision.ranked_count != 0U)) {
        return {AdaptiveOverlayApplyStatus::InvalidDecision, 0U};
    }
    const std::size_t assessment_count =
        static_cast<std::size_t>(decision.ranked_count) +
        decision.exclusion_count;
    if (assessment_count != views.size() ||
        views.size() > policy::kMaximumAdaptiveCandidates) {
        return {AdaptiveOverlayApplyStatus::CapacityMismatch, 0U};
    }

    for (std::size_t index{}; index < decision.ranked_count; ++index) {
        const policy::RankedCandidate& ranked = decision.ranked[index];
        if (!static_cast<bool>(ranked.venue) || ranked.pin.venue != ranked.venue ||
            ranked.pin.versions.fee_generation !=
                decision.versions.fee_generation ||
            ranked.pin.versions.metric_generation !=
                decision.versions.metric_generation ||
            ranked.executable_price.units <= 0) {
            return {AdaptiveOverlayApplyStatus::InvalidDecision, 0U};
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (decision.ranked[prior].venue == ranked.venue) {
                return {AdaptiveOverlayApplyStatus::AssessmentDuplicate, 0U};
            }
        }
    }
    for (std::size_t index{}; index < decision.exclusion_count; ++index) {
        const policy::PolicyExclusion& excluded = decision.exclusions[index];
        if (!static_cast<bool>(excluded.venue) ||
            excluded.pin.venue != excluded.venue ||
            excluded.reason == policy::PolicyExclusionReason::None) {
            return {AdaptiveOverlayApplyStatus::InvalidDecision, 0U};
        }
        for (std::size_t ranked{}; ranked < decision.ranked_count; ++ranked) {
            if (decision.ranked[ranked].venue == excluded.venue) {
                return {AdaptiveOverlayApplyStatus::AssessmentDuplicate, 0U};
            }
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (decision.exclusions[prior].venue == excluded.venue) {
                return {AdaptiveOverlayApplyStatus::AssessmentDuplicate, 0U};
            }
        }
    }

    for (std::size_t index{}; index < views.size(); ++index) {
        const BookSlotView& view = views[index];
        if (!view.valid() || view.instrument != decision.order.instrument) {
            return {AdaptiveOverlayApplyStatus::InvalidView, 0U};
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (views[prior].venue == view.venue) {
                return {AdaptiveOverlayApplyStatus::DuplicateVenue, 0U};
            }
        }
        std::size_t matches{};
        for (std::size_t ranked{}; ranked < decision.ranked_count; ++ranked) {
            matches += decision.ranked[ranked].venue == view.venue ? 1U : 0U;
        }
        for (std::size_t excluded{};
             excluded < decision.exclusion_count;
             ++excluded) {
            matches += decision.exclusions[excluded].venue == view.venue ? 1U : 0U;
        }
        if (matches != 1U) {
            return {matches == 0U
                        ? AdaptiveOverlayApplyStatus::AssessmentMissing
                        : AdaptiveOverlayApplyStatus::AssessmentDuplicate,
                    0U};
        }
    }

    std::array<AdaptivePolicyOverlay, policy::kMaximumAdaptiveCandidates>
        prepared{};
    for (std::size_t view_index{}; view_index < views.size(); ++view_index) {
        const BookSlotView& view = views[view_index];
        AdaptivePolicyOverlay overlay;
        overlay.versions = decision.versions;
        overlay.order = decision.order;
        overlay.applied = true;
        for (std::size_t ranked{}; ranked < decision.ranked_count; ++ranked) {
            if (decision.ranked[ranked].venue != view.venue) {
                continue;
            }
            overlay.fact_pin = decision.ranked[ranked].pin;
            overlay.evidence = decision.ranked[ranked];
            overlay.rank = static_cast<std::uint16_t>(ranked);
            overlay.eligible = true;
            break;
        }
        if (!overlay.eligible) {
            for (std::size_t excluded{};
                 excluded < decision.exclusion_count;
                 ++excluded) {
                if (decision.exclusions[excluded].venue != view.venue) {
                    continue;
                }
                overlay.fact_pin = decision.exclusions[excluded].pin;
                overlay.exclusion_reason = decision.exclusions[excluded].reason;
                break;
            }
        }
        if (!overlay.valid_shape()) {
            return {AdaptiveOverlayApplyStatus::InvalidDecision, 0U};
        }
        prepared[view_index] = overlay;
    }
    for (std::size_t index{}; index < views.size(); ++index) {
        views[index].adaptive_policy = prepared[index];
    }
    return {
        AdaptiveOverlayApplyStatus::Applied,
        static_cast<std::uint16_t>(views.size())};
}

// Cold-path compatibility helper for incrementally replacing
// CandidateRuntimeView publication with BookSlotPublisher.
[[nodiscard]] inline BookSlotView make_book_slot_view(
    const InstrumentId instrument,
    const CandidateRuntimeView& runtime,
    const std::uint64_t maximum_quote_age_ns,
    const std::uint64_t source_health_generation,
    const bool source_healthy,
    const std::uint64_t tx_credit_generation,
    const std::uint32_t normal_tx_credits,
    const std::uint32_t cancel_reserved_tx_credits,
    const std::uint64_t throttle_generation,
    const std::uint32_t throttle_credits) noexcept {
    BookSlotView output;
    output.venue = runtime.venue;
    output.instrument = instrument;
    output.adjacency_generation = runtime.adjacency_generation;
    output.market_data_state = runtime.market_data_state;
    output.order_entry_state = runtime.order_entry_state;
    output.egress_state = runtime.egress_state;
    output.instrument_state = runtime.instrument_state;
    output.conversion_compatible = runtime.conversion_compatible;
    output.risk_allowed = runtime.risk_allowed;
    output.maximum_quote_age_ns = maximum_quote_age_ns;
    output.source_health_generation = source_health_generation;
    output.source_healthy = source_healthy;
    output.tx_credit_generation = tx_credit_generation;
    output.normal_tx_credits = normal_tx_credits;
    output.cancel_reserved_tx_credits = cancel_reserved_tx_credits;
    output.throttle_generation = throttle_generation;
    output.throttle_credits = throttle_credits;
    output.has_book = runtime.book.has_value();
    if (output.has_book) {
        output.book = *runtime.book;
        output.feed_epoch = output.book.feed_session_epoch;
        output.monotonic_receive_time_ns = output.book.local_receive_time_ns;
        output.monotonic_publication_time_ns =
            output.book.local_publication_time_ns;
    }
    return output;
}

enum class BookPublishStatus : std::uint8_t {
    Published,
    InvalidView,
    ReaderBusy,
    GenerationExhausted,
};

// Data-race-free double-buffer publication.
//
// The low bit of published_token_ selects a buffer and the remaining bits are
// a monotonic publication generation. A tiny per-buffer try-lock closes the
// stale-reader race that makes a traditional C++ seqlock over non-atomic data
// undefined behaviour. Neither reader nor writer blocks or allocates. One
// thread owns publication; any number of readers may call try_read().
class BookSlotPublisher final {
public:
    BookSlotPublisher() noexcept = default;

    BookSlotPublisher(const BookSlotPublisher&) = delete;
    BookSlotPublisher& operator=(const BookSlotPublisher&) = delete;
    BookSlotPublisher(BookSlotPublisher&&) = delete;
    BookSlotPublisher& operator=(BookSlotPublisher&&) = delete;

    [[nodiscard]] BookPublishStatus try_publish(
        const BookSlotView& view) noexcept {
        if (!view.valid()) {
            return BookPublishStatus::InvalidView;
        }

        const std::uint64_t current = published_token_.load(
            std::memory_order_acquire);
        const std::uint64_t current_generation = current >> 1U;
        if (current_generation == maximum_generation()) {
            return BookPublishStatus::GenerationExhausted;
        }

        const std::size_t target = current == 0U ? 0U :
            static_cast<std::size_t>((current & 1U) ^ 1U);
        Buffer& buffer = buffers_[target];
        if (buffer.lock.test_and_set(std::memory_order_acquire)) {
            return BookPublishStatus::ReaderBusy;
        }

        buffer.view = view;
        const std::uint64_t next_generation = current_generation + 1U;
        const std::uint64_t next =
            (next_generation << 1U) | static_cast<std::uint64_t>(target);
        published_token_.store(next, std::memory_order_release);
        buffer.lock.clear(std::memory_order_release);
        return BookPublishStatus::Published;
    }

    [[nodiscard]] bool try_read(
        BookSlotView& output,
        std::uint64_t* publication_generation = nullptr,
        const std::size_t maximum_attempts = 4U) const noexcept {
        for (std::size_t attempt = 0U; attempt < maximum_attempts; ++attempt) {
            const std::uint64_t before = published_token_.load(
                std::memory_order_acquire);
            if (before == 0U) {
                return false;
            }
            const auto index = static_cast<std::size_t>(before & 1U);
            Buffer& buffer = buffers_[index];
            if (buffer.lock.test_and_set(std::memory_order_acquire)) {
                continue;
            }

            const std::uint64_t after = published_token_.load(
                std::memory_order_acquire);
            if (after != before) {
                buffer.lock.clear(std::memory_order_release);
                continue;
            }

            output = buffer.view;
            buffer.lock.clear(std::memory_order_release);
            if (publication_generation != nullptr) {
                *publication_generation = before >> 1U;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return published_token_.load(std::memory_order_acquire) >> 1U;
    }

private:
    [[nodiscard]] static constexpr std::uint64_t maximum_generation() noexcept {
        return std::numeric_limits<std::uint64_t>::max() >> 1U;
    }

    struct Buffer final {
        mutable std::atomic_flag lock = ATOMIC_FLAG_INIT;
        BookSlotView view{};
    };

    mutable std::array<Buffer, 2U> buffers_{};
    std::atomic<std::uint64_t> published_token_{};
};

}  // namespace sorx::dataplane
