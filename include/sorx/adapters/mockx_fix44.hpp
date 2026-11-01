#pragma once

#include "sorx/adapters/order_entry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sorx::adapters::mockx {

struct FixField final {
    int tag{};
    std::string value;

    friend bool operator==(const FixField&, const FixField&) = default;
};

struct FixMessage final {
    std::string message_type;
    std::vector<FixField> fields;

    friend bool operator==(const FixMessage&, const FixMessage&) = default;
};

struct MockXFix44Config final {
    std::uint8_t price_decimal_places{2U};
    std::uint8_t quantity_decimal_places{0U};
    std::size_t maximum_clordid_length{32U};
    std::string clordid_prefix{"SX"};
};

struct MessageBuildResult final {
    bool ok{};
    FixMessage message;
    std::string error;
};

struct NormalizeResult final {
    bool ok{};
    NormalizedExecutionReport report;
    std::string error;
};

struct MockXFix44WireIdentityBinding final {
    WireProfileIdentity identity;
    MockXFix44Config config;
};

class MockXFix44WireIdentityFactory final : public WireIdentityFactory {
public:
    explicit MockXFix44WireIdentityFactory(
        std::vector<MockXFix44WireIdentityBinding> bindings);

    [[nodiscard]] std::span<const WireProfileIdentity> profiles()
        const noexcept override;
    [[nodiscard]] WireIdentityResult make(
        const WireIdentityRequest& request) const override;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

class MockXFix44Profile final {
public:
    explicit MockXFix44Profile(MockXFix44Config config);

    [[nodiscard]] const MockXFix44Config& config() const noexcept;

    [[nodiscard]] std::string make_wire_order_id(
        ChildOrderId child,
        std::uint64_t parent_revision,
        std::uint64_t session_epoch,
        bool cancel) const;

    [[nodiscard]] bool valid_wire_order_id(std::string_view value) const noexcept;

    [[nodiscard]] MessageBuildResult make_new_order(
        const NewOrderIntent& intent) const;
    [[nodiscard]] MessageBuildResult make_cancel(
        const CancelOrderIntent& intent) const;

    [[nodiscard]] NormalizeResult normalize_execution_report(
        VenueId venue,
        std::uint64_t session_epoch,
        std::span<const FixField> fields) const;

    [[nodiscard]] NormalizeResult normalize_cancel_reject(
        VenueId venue,
        std::uint64_t session_epoch,
        std::uint64_t inbound_session_sequence,
        std::span<const FixField> fields) const;

private:
    MockXFix44Config config_;
};

}  // namespace sorx::adapters::mockx



