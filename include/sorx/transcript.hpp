#pragma once

#include "sorx/order_core.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace sorx {

struct TranscriptEvent {
    std::uint64_t ingress_sequence{0U};
    std::uint16_t kind{0U};
    std::vector<std::byte> payload;

    friend bool operator==(const TranscriptEvent&, const TranscriptEvent&) = default;
};

class TranscriptCodec {
public:
    static std::vector<std::byte> encode(std::span<const TranscriptEvent> events);
    static std::vector<TranscriptEvent> decode(std::span<const std::byte> bytes);
};

class TranscriptReplay {
public:
    using Apply = std::function<std::vector<std::byte>(const TranscriptEvent&)>;

    static std::vector<std::byte> run(
        std::span<const TranscriptEvent> input,
        const Apply& apply);
};

// Stable, protocol-neutral event tags for normalized coordinator input. These
// are deliberately independent of CoreEventPayload's std::variant index.
enum class CoreTranscriptEventKind : std::uint16_t {
    ParentSubmit = 1U,
    ParentCancel = 2U,
    ParentReplace = 3U,
    CandidateTablePublished = 4U,
    RuntimeViewPublished = 5U,
    OrderEntrySession = 6U,
    DispatchStarted = 7U,
    LocalDispatch = 8U,
    ExecutionReport = 9U,
    DispatchTimeout = 10U,
    KillSwitch = 11U,
    RoutingGate = 12U,
    Reconciliation = 13U,
};

// Canonical serialization for the complete normalized SorCore input surface.
// No raw venue-protocol or clock-duration fields are introduced here.
class CoreEventTranscriptCodec final {
public:
    static std::vector<std::byte> encode(std::span<const CoreEvent> events);
    static std::vector<CoreEvent> decode(std::span<const std::byte> bytes);
};

struct CoreReplayImage final {
    // One checksummed frame per input event. Each payload contains the
    // canonical ApplyResult followed by the canonical state image immediately
    // after that event, making decisions/accounting/effects/reports comparable
    // byte-for-byte at every replay step.
    std::vector<std::byte> outcome_transcript;
    std::vector<std::byte> final_state;

    friend bool operator==(const CoreReplayImage&, const CoreReplayImage&) = default;
};

class CoreTranscriptReplay final {
public:
    // Validates the complete transcript before applying its first event.
    static CoreReplayImage run(
        SorCore& core,
        std::span<const CoreEvent> events);
    static CoreReplayImage run_encoded(
        SorCore& core,
        std::span<const std::byte> transcript);
};

}  // namespace sorx


