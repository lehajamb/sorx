#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <type_traits>

namespace sorx {

template <typename Tag, typename Rep>
class StrongId final {
    static_assert(std::is_integral_v<Rep> && std::is_unsigned_v<Rep>);

public:
    using representation_type = Rep;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(const Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value_ != 0; }

    constexpr auto operator<=>(const StrongId&) const noexcept = default;

private:
    Rep value_{};
};

struct VenueIdTag;
struct InstrumentIdTag;
struct ParentOrderIdTag;
struct ChildOrderIdTag;
struct ChannelIdTag;
struct ProfileIdTag;

using VenueId = StrongId<VenueIdTag, std::uint32_t>;
using InstrumentId = StrongId<InstrumentIdTag, std::uint32_t>;
using ParentOrderId = StrongId<ParentOrderIdTag, std::uint64_t>;
using ChildOrderId = StrongId<ChildOrderIdTag, std::uint64_t>;
using ChannelId = StrongId<ChannelIdTag, std::uint32_t>;
using ProfileId = StrongId<ProfileIdTag, std::uint32_t>;

struct StrongIdHash final {
    template <typename Tag, typename Rep>
    [[nodiscard]] constexpr std::size_t operator()(const StrongId<Tag, Rep> id) const noexcept {
        return static_cast<std::size_t>(id.value());
    }
};

struct Price final {
    std::int64_t units{};

    explicit constexpr Price(const std::int64_t value = 0) noexcept : units(value) {}
    constexpr auto operator<=>(const Price&) const noexcept = default;
};

struct Quantity final {
    std::uint64_t units{};

    explicit constexpr Quantity(const std::uint64_t value = 0) noexcept : units(value) {}
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return units != 0; }
    constexpr auto operator<=>(const Quantity&) const noexcept = default;
};

struct Notional final {
    std::uint64_t units{};

    explicit constexpr Notional(const std::uint64_t value = 0) noexcept : units(value) {}
    constexpr auto operator<=>(const Notional&) const noexcept = default;
};

[[nodiscard]] constexpr std::optional<Quantity> checked_add(
    const Quantity lhs,
    const Quantity rhs) noexcept {
    if (rhs.units > std::numeric_limits<std::uint64_t>::max() - lhs.units) {
        return std::nullopt;
    }
    return Quantity{lhs.units + rhs.units};
}

[[nodiscard]] constexpr std::optional<Quantity> checked_subtract(
    const Quantity lhs,
    const Quantity rhs) noexcept {
    if (rhs.units > lhs.units) {
        return std::nullopt;
    }
    return Quantity{lhs.units - rhs.units};
}

[[nodiscard]] constexpr std::optional<Notional> checked_notional(
    const Price price,
    const Quantity quantity) noexcept {
    if (price.units < 0) {
        return std::nullopt;
    }
    const auto unsigned_price = static_cast<std::uint64_t>(price.units);
    if (unsigned_price != 0 &&
        quantity.units > std::numeric_limits<std::uint64_t>::max() / unsigned_price) {
        return std::nullopt;
    }
    return Notional{unsigned_price * quantity.units};
}

class RationalConversion final {
public:
    constexpr RationalConversion() noexcept = default;

    [[nodiscard]] static constexpr std::optional<RationalConversion> create(
        const std::uint64_t numerator,
        const std::uint64_t denominator) noexcept {
        constexpr auto signed_max = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        if (numerator == 0 || denominator == 0 ||
            numerator > signed_max || denominator > signed_max) {
            return std::nullopt;
        }
        const auto divisor = std::gcd(numerator, denominator);
        return RationalConversion{numerator / divisor, denominator / divisor};
    }

    [[nodiscard]] constexpr std::uint64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::uint64_t denominator() const noexcept { return denominator_; }

    [[nodiscard]] constexpr std::optional<Price> convert_exact(const Price source) const noexcept {
        const auto denominator = static_cast<std::int64_t>(denominator_);
        if (source.units % denominator != 0) {
            return std::nullopt;
        }

        const auto quotient = source.units / denominator;
        const auto numerator = static_cast<std::int64_t>(numerator_);
        if (quotient > 0 && quotient > std::numeric_limits<std::int64_t>::max() / numerator) {
            return std::nullopt;
        }
        if (quotient < 0 && quotient < std::numeric_limits<std::int64_t>::min() / numerator) {
            return std::nullopt;
        }
        return Price{quotient * numerator};
    }

    [[nodiscard]] constexpr std::optional<Quantity> convert_exact(
        const Quantity source) const noexcept {
        if (source.units % denominator_ != 0) {
            return std::nullopt;
        }
        const auto quotient = source.units / denominator_;
        if (quotient > std::numeric_limits<std::uint64_t>::max() / numerator_) {
            return std::nullopt;
        }
        return Quantity{quotient * numerator_};
    }

    constexpr auto operator<=>(const RationalConversion&) const noexcept = default;

private:
    constexpr RationalConversion(
        const std::uint64_t numerator,
        const std::uint64_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::uint64_t numerator_{1};
    std::uint64_t denominator_{1};
};

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class RoutingIntent : std::uint8_t {
    AggressiveIOC,
    PassiveGTC,
};

enum class AdminState : std::uint8_t {
    Up,
    Down,
};

enum class MarketDataState : std::uint8_t {
    Snapshotting,
    Stitching,
    Live,
    Recovering,
    Failed,
};

enum class OrderEntryState : std::uint8_t {
    Disconnected,
    Connecting,
    Ready,
    Degraded,
};

enum class EgressState : std::uint8_t {
    Ready,
    Congested,
};

enum class InstrumentState : std::uint8_t {
    Unknown,
    Trading,
    Halted,
    Closed,
};

struct ParentOrder final {
    ParentOrderId id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price limit_price{};
    RoutingIntent intent{RoutingIntent::AggressiveIOC};

    constexpr auto operator<=>(const ParentOrder&) const noexcept = default;
};

struct ParentReplace final {
    ParentOrderId id{};
    Quantity new_total_quantity{};
    Price new_limit_price{};

    constexpr auto operator<=>(const ParentReplace&) const noexcept = default;
};

}  // namespace sorx
