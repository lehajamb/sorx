#pragma once

#include "sorx/recorder/flight_recorder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace sorx::fastpath {

inline constexpr std::size_t kMaximumPacketBurst = 64U;
inline constexpr std::size_t kMaximumEncodedFrameBytes = 512U;
inline constexpr std::size_t kAttemptReplayWindow = 4'096U;

struct RegisteredPacket final {
    const std::byte* data{};
    std::uint32_t length{};
    std::uint16_t receive_queue{};
    std::uint64_t hardware_receive_time_ns{};
    std::uint64_t source_sequence{};

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data, length};
    }
};

struct PacketBurst final {
    std::array<RegisteredPacket, kMaximumPacketBurst> packets{};
    std::uint16_t count{};

    [[nodiscard]] bool valid() const noexcept {
        if (count > packets.size()) {
            return false;
        }
        for (std::size_t index{}; index < count; ++index) {
            if (packets[index].data == nullptr || packets[index].length == 0U) {
                return false;
            }
        }
        return true;
    }
};

class IMarketDataRx {
public:
    virtual ~IMarketDataRx() = default;
    [[nodiscard]] virtual PacketBurst poll() noexcept = 0;
};

enum class DispatchClass : std::uint8_t {
    NewOrReplace,
    CancelOrReconcile,
};

struct EncodedFrame final {
    std::uint32_t venue_id{};
    std::uint64_t child_id{};
    std::uint64_t dispatch_attempt_id{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    std::uint64_t ownership_generation{};
    DispatchClass dispatch_class{DispatchClass::NewOrReplace};
    std::uint16_t size{};
    std::array<std::byte, kMaximumEncodedFrameBytes> storage{};

    [[nodiscard]] bool valid() const noexcept {
        return venue_id != 0U && child_id != 0U &&
            dispatch_attempt_id != 0U && leadership_epoch != 0U &&
            configuration_generation != 0U && venue_session_id != 0U &&
            venue_session_epoch != 0U && ownership_generation != 0U &&
            size != 0U &&
            size <= storage.size();
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {storage.data(), size};
    }
};

static_assert(std::is_trivially_copyable_v<EncodedFrame>);

enum class SendResult : std::uint8_t {
    Sent,
    WouldBlock,
    InvalidFrame,
    WrongVenue,
    MissingOrInvalidDurableToken,
    DuplicateOrExpiredAttempt,
    TransportClosed,
    TransportFailure,
};

class IOrderEntrySession {
public:
    virtual ~IOrderEntrySession() = default;
    [[nodiscard]] virtual SendResult try_send(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept = 0;
};

enum class TransportWriteResult : std::uint8_t {
    Sent,
    WouldBlock,
    Closed,
    Failed,
};

// Production implementations own a registered NIC queue or FPGA descriptor
// aperture. The recorder gate calls this interface only after all token checks
// have succeeded.
class IFrameTransport {
public:
    virtual ~IFrameTransport() = default;
    [[nodiscard]] virtual TransportWriteResult try_transmit(
        std::span<const std::byte> frame) noexcept = 0;
};

struct TokenGatePolicy final {
    std::uint32_t venue_id{};
    recorder::DurabilityDomain required_domain{
        recorder::DurabilityDomain::HardwarePersistent};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
};

// Allocation-free software enforcement of the final pre-wire invariant.
// A production FPGA/NIC backend must independently enforce the same identity
// and token checks before it can assert a transmit descriptor.
class TokenGatedOrderEntrySession final : public IOrderEntrySession {
public:
    TokenGatedOrderEntrySession(
        TokenGatePolicy policy,
        const recorder::IFlightRecorder& recorder,
        IFrameTransport& transport) noexcept;

    [[nodiscard]] SendResult try_send(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept override;

    [[nodiscard]] std::uint64_t last_transmitted_attempt_id() const noexcept;
    [[nodiscard]] std::uint64_t highest_transmitted_attempt_id() const noexcept;

private:
    [[nodiscard]] bool attempt_replayed_or_too_old(
        std::uint64_t attempt_id) const noexcept;
    void mark_attempt_transmitted(std::uint64_t attempt_id) noexcept;

    TokenGatePolicy policy_{};
    const recorder::IFlightRecorder* recorder_{};
    IFrameTransport* transport_{};
    std::uint64_t last_transmitted_attempt_id_{};
    std::uint64_t highest_transmitted_attempt_id_{};
    std::array<std::uint64_t, kAttemptReplayWindow> transmitted_attempts_{};
};

}  // namespace sorx::fastpath
