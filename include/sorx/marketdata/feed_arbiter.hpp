#pragma once

#include <cstdint>

namespace sorx::marketdata {

enum class FeedLeg : std::uint8_t {
    None,
    A,
    B,
};

enum class FeedArbiterState : std::uint8_t {
    AwaitingSnapshot,
    Live,
    Recovering,
    Failed,
};

enum class FeedHealth : std::uint8_t {
    Healthy,
    AwaitingSnapshot,
    Recovering,
    Failed,
    HeartbeatsStalled,
    QuoteStale,
    GapPending,
    ClockRegression,
};

struct FeedArbiterConfig final {
    std::uint64_t heartbeat_timeout_ns{};
    std::uint64_t maximum_quote_age_ns{};
};

struct FeedSnapshotCut final {
    FeedLeg source{FeedLeg::None};
    std::uint64_t feed_epoch{};
    std::uint64_t next_sequence{};
    std::uint64_t local_receive_time_ns{};
    std::uint64_t last_quote_receive_time_ns{};
};

struct FeedPacketObservation final {
    FeedLeg leg{FeedLeg::None};
    std::uint64_t feed_epoch{};
    std::uint64_t first_sequence{};
    std::uint16_t message_count{};
    std::uint64_t local_receive_time_ns{};
    bool contains_quote{};
};

enum class ArbitrationAction : std::uint8_t {
    Apply,
    FailoverApply,
    HeartbeatAccepted,
    DuplicateIgnored,
    AwaitPeer,
    RecoveryRequired,
    EpochRollover,
    StaleEpochIgnored,
    AwaitingSnapshot,
    PendingCommit,
    Malformed,
    Failed,
};

struct ArbitrationTicket final {
    std::uint64_t nonce{};
    std::uint64_t feed_epoch{};
    std::uint64_t first_sequence{};
    std::uint64_t next_sequence{};
    std::uint64_t local_receive_time_ns{};
    FeedLeg leg{FeedLeg::None};
    bool contains_quote{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return nonce != 0U && leg != FeedLeg::None &&
               next_sequence > first_sequence;
    }
};

struct ArbitrationResult final {
    ArbitrationAction action{ArbitrationAction::Malformed};
    ArbitrationTicket ticket;
    FeedHealth health{FeedHealth::AwaitingSnapshot};

    [[nodiscard]] bool should_apply() const noexcept {
        return action == ArbitrationAction::Apply ||
               action == ArbitrationAction::FailoverApply;
    }
};

enum class CommitStatus : std::uint8_t {
    Committed,
    ApplicationRejectedRecoveryRequired,
    InvalidTicket,
};

struct FeedArbiterMetadata final {
    FeedArbiterState state{FeedArbiterState::AwaitingSnapshot};
    FeedLeg selected_leg{FeedLeg::None};
    std::uint64_t feed_epoch{};
    std::uint64_t recovery_generation{1U};
    std::uint64_t next_sequence{};
    std::uint64_t last_quote_receive_time_ns{};
    bool commit_pending{};
};

class FeedArbiter final {
public:
    explicit FeedArbiter(FeedArbiterConfig config);

    [[nodiscard]] bool install_snapshot(
        const FeedSnapshotCut& snapshot) noexcept;
    [[nodiscard]] ArbitrationResult inspect(
        const FeedPacketObservation& packet) noexcept;
    [[nodiscard]] CommitStatus commit(
        const ArbitrationTicket& ticket,
        bool application_succeeded) noexcept;
    [[nodiscard]] FeedHealth poll(std::uint64_t now_ns) noexcept;
    void force_recovery() noexcept;
    void fail() noexcept;

    [[nodiscard]] FeedHealth health(std::uint64_t now_ns) const noexcept;
    [[nodiscard]] bool may_publish(std::uint64_t now_ns) const noexcept;
    [[nodiscard]] FeedArbiterMetadata metadata() const noexcept;

private:
    struct LegState final {
        std::uint64_t last_observation_time_ns{};
        std::uint64_t gap_first_sequence{};
        bool seen{};
    };

    [[nodiscard]] bool leg_healthy(
        FeedLeg leg,
        std::uint64_t now_ns) const noexcept;
    [[nodiscard]] LegState& leg(FeedLeg value) noexcept;
    [[nodiscard]] const LegState& leg(FeedLeg value) const noexcept;
    void enter_recovery() noexcept;

    FeedArbiterConfig config_;
    LegState leg_a_;
    LegState leg_b_;
    FeedArbiterState state_{FeedArbiterState::AwaitingSnapshot};
    FeedLeg selected_leg_{FeedLeg::None};
    std::uint64_t feed_epoch_{};
    std::uint64_t recovery_generation_{1U};
    std::uint64_t next_sequence_{};
    std::uint64_t last_quote_receive_time_ns_{};
    std::uint64_t last_polled_time_ns_{};
    std::uint64_t next_nonce_{1U};
    ArbitrationTicket pending_ticket_{};
};

}  // namespace sorx::marketdata
