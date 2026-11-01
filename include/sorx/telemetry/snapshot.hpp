#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace sorx::telemetry {

inline constexpr std::size_t kMaximumTelemetryVenues = 64U;
inline constexpr std::size_t kLatencyBucketCount = 32U;
inline constexpr std::size_t kStageHistogramCount = 8U;

enum class ReadinessState : std::uint8_t {
    Bootstrapping,
    RecoveryOnly,
    Ready,
    Degraded,
    Failed,
};

enum class Stage : std::uint8_t {
    IngressDecode,
    RouteAndRisk,
    RecorderCommit,
    RouteCommit,
    TxEnqueue,
    TxEncode,
    NicSubmit,
    EndToEnd,
};

struct LatencyHistogram final {
    // Bucket n covers [2^n, 2^(n+1)) nanoseconds; bucket 31 saturates.
    std::array<std::uint64_t, kLatencyBucketCount> buckets{};
    std::uint64_t samples{};
    std::uint64_t maximum_ns{};

    void observe(const std::uint64_t nanoseconds) noexcept {
        std::size_t bucket{};
        std::uint64_t value = nanoseconds;
        while (value > 1U && bucket + 1U < buckets.size()) {
            value >>= 1U;
            ++bucket;
        }
        ++buckets[bucket];
        ++samples;
        if (nanoseconds > maximum_ns) {
            maximum_ns = nanoseconds;
        }
    }
};

struct VenueSnapshot final {
    std::uint32_t venue_id{};
    std::uint32_t normal_tx_depth{};
    std::uint32_t normal_tx_high_water{};
    std::uint32_t cancel_tx_depth{};
    std::uint32_t cancel_tx_high_water{};
    std::uint32_t normal_credits{};
    std::uint32_t cancel_credits{};
    std::uint64_t feed_age_ns{};
    std::uint64_t feed_gaps{};
    std::uint64_t feed_failovers{};
    std::uint64_t throttle_rejections{};
    std::uint64_t order_entry_rejections{};
    std::uint8_t market_data_state{};
    std::uint8_t order_entry_state{};
    bool routable{};
};

struct Snapshot final {
    std::uint64_t generation{};
    std::uint64_t monotonic_time_ns{};
    std::uint64_t tai_time_ns{};
    std::uint64_t configuration_generation{};
    std::uint64_t leadership_epoch{};
    ReadinessState readiness{ReadinessState::Bootstrapping};
    bool routing_enabled{};
    bool kill_switch_active{true};
    bool ptp_locked{};
    std::int64_t ptp_offset_ns{};
    std::uint64_t nic_rx_drops{};
    std::uint64_t nic_tx_drops{};
    std::uint64_t ingress_depth{};
    std::uint64_t ingress_high_water{};
    std::uint64_t active_parents{};
    std::uint64_t active_children{};
    std::uint64_t risk_notional_used{};
    std::uint64_t tier0_used_bytes{};
    std::uint64_t tier0_capacity_bytes{};
    std::uint64_t tier1_lag_bytes{};
    std::uint64_t audit_earliest_sequence{};
    std::uint64_t audit_latest_sequence{};
    std::uint32_t cpu_temperature_millicelsius{};
    std::uint32_t venue_count{};
    std::array<VenueSnapshot, kMaximumTelemetryVenues> venues{};
    std::array<LatencyHistogram, kStageHistogramCount> stage_histograms{};

    [[nodiscard]] bool valid() const noexcept {
        return generation != 0U && monotonic_time_ns != 0U &&
            venue_count <= venues.size();
    }
};

static_assert(std::is_trivially_copyable_v<Snapshot>);

// A dataplane owner publishes complete values. Management readers only copy a
// stable buffer and therefore never lock, call, or walk coordinator state.
class SnapshotPublisher final {
public:
    SnapshotPublisher() noexcept = default;
    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

    [[nodiscard]] bool try_publish(const Snapshot& value) noexcept {
        if (!value.valid()) {
            return false;
        }
        const std::uint64_t current =
            publication_.value.load(std::memory_order_acquire);
        const std::uint64_t generation = current >> 1U;
        if (generation == (std::numeric_limits<std::uint64_t>::max() >> 1U)) {
            return false;
        }
        const std::size_t target = current == 0U
            ? 0U
            : static_cast<std::size_t>((current & 1U) ^ 1U);
        Buffer& buffer = buffers_[target];
        if (buffer.lock.test_and_set(std::memory_order_acquire)) {
            return false;
        }
        buffer.value = value;
        publication_.value.store(
            ((generation + 1U) << 1U) | static_cast<std::uint64_t>(target),
            std::memory_order_release);
        buffer.lock.clear(std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_read(
        Snapshot& output,
        const std::size_t maximum_attempts = 4U) const noexcept {
        for (std::size_t attempt{}; attempt < maximum_attempts; ++attempt) {
            const std::uint64_t before =
                publication_.value.load(std::memory_order_acquire);
            if (before == 0U) {
                return false;
            }
            Buffer& buffer = buffers_[static_cast<std::size_t>(before & 1U)];
            if (buffer.lock.test_and_set(std::memory_order_acquire)) {
                continue;
            }
            const std::uint64_t after =
                publication_.value.load(std::memory_order_acquire);
            if (before != after) {
                buffer.lock.clear(std::memory_order_release);
                continue;
            }
            output = buffer.value;
            buffer.lock.clear(std::memory_order_release);
            return true;
        }
        return false;
    }

private:
    struct Buffer final {
        mutable std::atomic_flag lock = ATOMIC_FLAG_INIT;
        Snapshot value{};
    };

    struct Publication final {
        std::atomic<std::uint64_t> value{};
    };

    mutable std::array<Buffer, 2U> buffers_{};
    Publication publication_{};
};

}  // namespace sorx::telemetry
