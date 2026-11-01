#include "sorx/risk/risk_kernel.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sorx::risk {
namespace {

enum class SlotState : std::uint8_t {
    Empty,
    Occupied,
    Tombstone,
};

[[nodiscard]] constexpr bool valid_permissions(
    const RiskPermission value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(RiskPermission::NewAndCancel);
}

[[nodiscard]] constexpr bool valid_side(const Side value) noexcept {
    return value == Side::Buy || value == Side::Sell;
}

[[nodiscard]] constexpr bool valid_verdict(
    const RiskVerdict value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(
               RiskVerdict::DedupeCapacityExceeded);
}

[[nodiscard]] bool digest_present(const PayloadDigest& digest) noexcept {
    return std::any_of(digest.begin(), digest.end(), [](const std::byte value) {
        return value != std::byte{};
    });
}

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr std::size_t hash_index(
    const std::uint64_t key,
    const std::size_t capacity) noexcept {
    return static_cast<std::size_t>(mix64(key) % capacity);
}

class HashBuilder final {
public:
    explicit HashBuilder(const std::uint64_t seed) noexcept : value_(seed) {}

    void add(const std::uint64_t value) noexcept {
        value_ = mix64(value_ ^ mix64(value + 0x9e3779b97f4a7c15ULL));
    }

    void add_digest(const PayloadDigest& digest) noexcept {
        std::uint64_t part = 0U;
        for (std::size_t index = 0U; index < digest.size(); ++index) {
            part = (part << 8U) | std::to_integer<std::uint8_t>(digest[index]);
            if ((index & 7U) == 7U) {
                add(part);
                part = 0U;
            }
        }
    }

    [[nodiscard]] std::uint64_t finish() const noexcept {
        return value_ == 0U ? 0xd6e8feb86659fd93ULL : value_;
    }

private:
    std::uint64_t value_;
};

void add_command(HashBuilder& hash, const RiskCommand& command) noexcept {
    hash.add(static_cast<std::uint8_t>(command.kind));
    hash.add(command.request_id);
    hash.add_digest(command.payload_digest);
    hash.add(command.client_id);
    hash.add(command.account_id);
    hash.add(command.parent_order_id.value());
    hash.add(command.instrument.value());
    hash.add(static_cast<std::uint8_t>(command.side));
    hash.add(command.quantity.units);
    hash.add(static_cast<std::uint64_t>(command.limit_price.units));
}

[[nodiscard]] std::uint64_t command_fingerprint(
    const RiskCommand& command) noexcept {
    HashBuilder hash{0x5249534b434d4431ULL};
    add_command(hash, command);
    return hash.finish();
}

[[nodiscard]] bool limits_valid(const RiskLimits& limits) noexcept {
    return limits.maximum_single_order_quantity.units != 0U &&
           limits.maximum_single_order_notional.units != 0U &&
           limits.maximum_open_quantity.units != 0U &&
           limits.maximum_open_notional.units != 0U &&
           limits.maximum_open_orders != 0U &&
           limits.minimum_price.units > 0 &&
           limits.maximum_price >= limits.minimum_price &&
           limits.maximum_single_order_quantity <=
               limits.maximum_open_quantity &&
           limits.maximum_single_order_notional <=
               limits.maximum_open_notional;
}

[[nodiscard]] bool bucket_valid(const TokenBucketConfig& bucket) noexcept {
    return bucket.capacity != 0U && bucket.refill_tokens != 0U &&
           bucket.refill_interval_ns != 0U;
}

struct BucketPreview final {
    std::uint32_t tokens{};
    std::uint64_t refill_time_ns{};
    bool token_available{};
};

}  // namespace

struct RiskKernel::Impl final {
    struct BucketState final {
        TokenBucketConfig config;
        std::uint32_t tokens{};
        std::uint64_t refill_time_ns{};
    };

    struct AccountSlot final {
        SlotState state{SlotState::Empty};
        AccountId id{};
        RiskPermission permissions{RiskPermission::None};
        RiskLimits limits;
        Quantity open_quantity{};
        Notional open_notional{};
        std::uint32_t open_orders{};
    };

    struct ClientSlot final {
        SlotState state{SlotState::Empty};
        ClientId id{};
        AccountId account_id{};
        RiskPermission permissions{RiskPermission::None};
        RiskLimits limits;
        BucketState normal;
        BucketState cancel;
        Quantity open_quantity{};
        Notional open_notional{};
        std::uint32_t open_orders{};
    };

    struct ParentSlot final {
        SlotState state{SlotState::Empty};
        ParentOrderId id{};
        ClientId client_id{};
        AccountId account_id{};
        InstrumentId instrument{};
        Quantity remaining_quantity{};
        Notional remaining_notional{};
        Price limit_price{};
    };

    struct DedupeSlot final {
        SlotState state{SlotState::Empty};
        ClientId client_id{};
        RequestId request_id{};
        std::uint64_t fingerprint{};
        RiskVerdict verdict{RiskVerdict::InvalidCommand};
        Quantity reservation_quantity{};
        Notional reservation_notional{};
    };

    struct RestrictedSlot final {
        SlotState state{SlotState::Empty};
        InstrumentId instrument{};
    };

    struct Probe final {
        bool found{};
        bool insertable{};
        std::size_t index{};
    };

    Impl(
        const RiskKernelConfig supplied,
        const std::span<const AccountRiskConfig> supplied_accounts,
        const std::span<const ClientRiskConfig> supplied_clients,
        const std::span<const InstrumentId> supplied_restricted)
        : config(supplied),
          accounts(std::make_unique<AccountSlot[]>(supplied.account_capacity)),
          clients(std::make_unique<ClientSlot[]>(supplied.client_capacity)),
          parents(std::make_unique<ParentSlot[]>(
              supplied.active_parent_capacity)),
          dedupe(std::make_unique<DedupeSlot[]>(supplied.dedupe_capacity)),
          restricted(supplied.restricted_instrument_capacity == 0U
                         ? nullptr
                         : std::make_unique<RestrictedSlot[]>(
                               supplied.restricted_instrument_capacity)),
          configured_account_count(supplied_accounts.size()),
          configured_client_count(supplied_clients.size()) {
        for (const AccountRiskConfig& account : supplied_accounts) {
            const Probe location = probe_account(account.account_id);
            if (!location.insertable || location.found) {
                throw std::invalid_argument("duplicate account risk identity");
            }
            accounts[location.index] = AccountSlot{SlotState::Occupied,
                                                    account.account_id,
                                                    account.permissions,
                                                    account.limits};
        }
        for (const ClientRiskConfig& client : supplied_clients) {
            const Probe location = probe_client(client.client_id);
            if (!location.insertable || location.found ||
                find_account(client.account_id) == nullptr) {
                throw std::invalid_argument(
                    "duplicate client or unknown client account");
            }
            clients[location.index] = ClientSlot{
                SlotState::Occupied,
                client.client_id,
                client.account_id,
                client.permissions,
                client.limits,
                BucketState{client.normal_rate,
                            client.normal_rate.capacity, 0U},
                BucketState{client.cancel_rate,
                            client.cancel_rate.capacity, 0U}};
        }
        for (const InstrumentId instrument : supplied_restricted) {
            const Probe location = probe_restricted(instrument);
            if (!location.insertable || location.found) {
                throw std::invalid_argument("duplicate restricted instrument");
            }
            restricted[location.index] =
                RestrictedSlot{SlotState::Occupied, instrument};
        }
    }

    [[nodiscard]] Probe probe_account(const AccountId id) const noexcept {
        const std::size_t start = hash_index(id, config.account_capacity);
        for (std::size_t offset = 0U; offset < config.account_capacity;
             ++offset) {
            const std::size_t index =
                (start + offset) % config.account_capacity;
            const AccountSlot& slot = accounts[index];
            if (slot.state == SlotState::Empty) {
                return {false, true, index};
            }
            if (slot.state == SlotState::Occupied && slot.id == id) {
                return {true, true, index};
            }
        }
        return {};
    }

    [[nodiscard]] Probe probe_client(const ClientId id) const noexcept {
        const std::size_t start = hash_index(id, config.client_capacity);
        for (std::size_t offset = 0U; offset < config.client_capacity;
             ++offset) {
            const std::size_t index =
                (start + offset) % config.client_capacity;
            const ClientSlot& slot = clients[index];
            if (slot.state == SlotState::Empty) {
                return {false, true, index};
            }
            if (slot.state == SlotState::Occupied && slot.id == id) {
                return {true, true, index};
            }
        }
        return {};
    }

    [[nodiscard]] Probe probe_parent(const ParentOrderId id) const noexcept {
        const std::size_t start =
            hash_index(id.value(), config.active_parent_capacity);
        std::size_t tombstone = config.active_parent_capacity;
        for (std::size_t offset = 0U; offset < config.active_parent_capacity;
             ++offset) {
            const std::size_t index =
                (start + offset) % config.active_parent_capacity;
            const ParentSlot& slot = parents[index];
            if (slot.state == SlotState::Occupied && slot.id == id) {
                return {true, true, index};
            }
            if (slot.state == SlotState::Tombstone &&
                tombstone == config.active_parent_capacity) {
                tombstone = index;
            }
            if (slot.state == SlotState::Empty) {
                return {false, true,
                        tombstone == config.active_parent_capacity ? index
                                                                   : tombstone};
            }
        }
        return tombstone == config.active_parent_capacity
                   ? Probe{}
                   : Probe{false, true, tombstone};
    }

    [[nodiscard]] Probe probe_dedupe(
        const ClientId client,
        const RequestId request) const noexcept {
        const std::uint64_t key = mix64(client) ^ request;
        const std::size_t start = hash_index(key, config.dedupe_capacity);
        std::size_t tombstone = config.dedupe_capacity;
        for (std::size_t offset = 0U; offset < config.dedupe_capacity;
             ++offset) {
            const std::size_t index =
                (start + offset) % config.dedupe_capacity;
            const DedupeSlot& slot = dedupe[index];
            if (slot.state == SlotState::Occupied &&
                slot.client_id == client && slot.request_id == request) {
                return {true, true, index};
            }
            if (slot.state == SlotState::Tombstone &&
                tombstone == config.dedupe_capacity) {
                tombstone = index;
            }
            if (slot.state == SlotState::Empty) {
                return {false, true,
                        tombstone == config.dedupe_capacity ? index
                                                            : tombstone};
            }
        }
        return tombstone == config.dedupe_capacity
                   ? Probe{}
                   : Probe{false, true, tombstone};
    }

    [[nodiscard]] Probe probe_restricted(
        const InstrumentId instrument) const noexcept {
        if (config.restricted_instrument_capacity == 0U) {
            return {};
        }
        const std::size_t start = hash_index(
            instrument.value(), config.restricted_instrument_capacity);
        for (std::size_t offset = 0U;
             offset < config.restricted_instrument_capacity; ++offset) {
            const std::size_t index =
                (start + offset) % config.restricted_instrument_capacity;
            const RestrictedSlot& slot = restricted[index];
            if (slot.state == SlotState::Empty) {
                return {false, true, index};
            }
            if (slot.state == SlotState::Occupied &&
                slot.instrument == instrument) {
                return {true, true, index};
            }
        }
        return {};
    }

    [[nodiscard]] AccountSlot* find_account(const AccountId id) noexcept {
        const Probe result = probe_account(id);
        return result.found ? &accounts[result.index] : nullptr;
    }

    [[nodiscard]] const AccountSlot* find_account(
        const AccountId id) const noexcept {
        const Probe result = probe_account(id);
        return result.found ? &accounts[result.index] : nullptr;
    }

    [[nodiscard]] ClientSlot* find_client(const ClientId id) noexcept {
        const Probe result = probe_client(id);
        return result.found ? &clients[result.index] : nullptr;
    }

    [[nodiscard]] const ClientSlot* find_client(
        const ClientId id) const noexcept {
        const Probe result = probe_client(id);
        return result.found ? &clients[result.index] : nullptr;
    }

    [[nodiscard]] const ParentSlot* find_parent(
        const ParentOrderId id) const noexcept {
        const Probe result = probe_parent(id);
        return result.found ? &parents[result.index] : nullptr;
    }

    [[nodiscard]] BucketPreview preview_bucket(
        const BucketState& bucket,
        const std::uint64_t now_ns,
        const bool consume) const noexcept {
        std::uint32_t tokens = bucket.tokens;
        std::uint64_t refill_time = bucket.refill_time_ns;
        if (refill_time == 0U) {
            refill_time = now_ns;
            tokens = bucket.config.capacity;
        } else if (now_ns > refill_time) {
            const std::uint64_t periods =
                (now_ns - refill_time) / bucket.config.refill_interval_ns;
            if (periods != 0U) {
                const std::uint32_t missing = bucket.config.capacity - tokens;
                const std::uint64_t periods_to_fill =
                    (static_cast<std::uint64_t>(missing) +
                     bucket.config.refill_tokens - 1U) /
                    bucket.config.refill_tokens;
                if (periods >= periods_to_fill) {
                    tokens = bucket.config.capacity;
                } else {
                    tokens += static_cast<std::uint32_t>(
                        periods * bucket.config.refill_tokens);
                }
                refill_time += periods * bucket.config.refill_interval_ns;
            }
        }
        const bool available = tokens != 0U;
        if (consume && available) {
            --tokens;
        }
        return {tokens, refill_time, available};
    }

    [[nodiscard]] std::uint64_t authenticate(
        const RiskDelta& delta) const noexcept {
        HashBuilder hash{config.delta_authentication_key[0] ^
                         0x44454c54414d4143ULL};
        hash.add(config.delta_authentication_key[1]);
        hash.add(static_cast<std::uint8_t>(delta.disposition));
        hash.add(static_cast<std::uint8_t>(delta.verdict));
        add_command(hash, delta.command);
        hash.add(delta.command_fingerprint);
        hash.add(delta.risk_generation);
        hash.add(delta.configuration_generation);
        hash.add(delta.kernel_state_generation);
        hash.add(delta.monotonic_time_ns);
        hash.add(delta.reservation_quantity.units);
        hash.add(delta.reservation_notional.units);
        hash.add(delta.normal_tokens_after);
        hash.add(delta.cancel_tokens_after);
        hash.add(delta.normal_refill_time_after);
        hash.add(delta.cancel_refill_time_after);
        return hash.finish();
    }

    [[nodiscard]] RiskPlanResult finish_plan(
        RiskDelta delta,
        const RiskPlanDisposition disposition,
        const RiskVerdict verdict,
        const bool authenticate_delta) const noexcept {
        delta.disposition = disposition;
        delta.verdict = verdict;
        if (authenticate_delta) {
            delta.authentication_tag = authenticate(delta);
        }
        return {disposition, verdict, delta};
    }

    [[nodiscard]] RiskPlanResult plan(
        const RiskCommand& command,
        const std::uint64_t time_ns) const noexcept {
        RiskDelta delta{};
        delta.command = command;
        delta.command_fingerprint = command_fingerprint(command);
        delta.risk_generation = config.risk_generation;
        delta.configuration_generation = config.configuration_generation;
        delta.kernel_state_generation = state_generation;
        delta.monotonic_time_ns = time_ns;

        if (time_ns == 0U || time_ns < last_committed_time_ns ||
            command.request_id == 0U || command.client_id == 0U ||
            command.account_id == 0U || !command.parent_order_id ||
            !digest_present(command.payload_digest)) {
            const RiskVerdict verdict =
                time_ns != 0U && time_ns < last_committed_time_ns
                    ? RiskVerdict::ClockRegression
                    : RiskVerdict::InvalidCommand;
            return finish_plan(delta, RiskPlanDisposition::Invalid, verdict,
                               false);
        }

        const Probe dedupe_location =
            probe_dedupe(command.client_id, command.request_id);
        if (dedupe_location.found) {
            const DedupeSlot& prior = dedupe[dedupe_location.index];
            if (prior.fingerprint != delta.command_fingerprint) {
                return finish_plan(delta,
                                   RiskPlanDisposition::DuplicateConflict,
                                   RiskVerdict::InvalidCommand, false);
            }
            delta.reservation_quantity = prior.reservation_quantity;
            delta.reservation_notional = prior.reservation_notional;
            return finish_plan(delta, RiskPlanDisposition::DuplicateReplay,
                               prior.verdict, false);
        }
        if (!dedupe_location.insertable) {
            return finish_plan(delta, RiskPlanDisposition::CapacityExceeded,
                               RiskVerdict::DedupeCapacityExceeded, false);
        }

        const ClientSlot* const client = find_client(command.client_id);
        const AccountSlot* const account = find_account(command.account_id);
        bool normal_token_available = false;
        bool cancel_token_available = false;
        RiskVerdict verdict = RiskVerdict::Allowed;
        if (client == nullptr) {
            verdict = RiskVerdict::UnknownClient;
        } else if (account == nullptr) {
            verdict = RiskVerdict::UnknownAccount;
        } else if (client->account_id != command.account_id) {
            verdict = RiskVerdict::ClientAccountMismatch;
        }

        if (client != nullptr) {
            const bool cancel = command.kind == RiskCommandKind::Cancel;
            const BucketPreview normal = preview_bucket(
                client->normal, time_ns, !cancel);
            const BucketPreview cancel_preview = preview_bucket(
                client->cancel, time_ns, cancel);
            delta.normal_tokens_after = normal.tokens;
            delta.cancel_tokens_after = cancel_preview.tokens;
            delta.normal_refill_time_after = normal.refill_time_ns;
            delta.cancel_refill_time_after = cancel_preview.refill_time_ns;
            normal_token_available = normal.token_available;
            cancel_token_available = cancel_preview.token_available;
        }

        if (verdict == RiskVerdict::Allowed) {
            const RiskPermission required =
                command.kind == RiskCommandKind::NewOrder
                    ? RiskPermission::NewOrder
                    : RiskPermission::Cancel;
            if (!permits(client->permissions, required)) {
                verdict = RiskVerdict::ClientPermissionDenied;
            } else if (!permits(account->permissions, required)) {
                verdict = RiskVerdict::AccountPermissionDenied;
            }
        }

        if (command.kind == RiskCommandKind::NewOrder) {
            if (verdict == RiskVerdict::Allowed && kill_switch) {
                verdict = RiskVerdict::KillSwitchEngaged;
            }
            if (verdict == RiskVerdict::Allowed &&
                (!command.instrument || !valid_side(command.side) ||
                 !command.quantity || command.limit_price.units <= 0)) {
                verdict = RiskVerdict::InvalidCommand;
            }
            if (verdict == RiskVerdict::Allowed &&
                probe_restricted(command.instrument).found) {
                verdict = RiskVerdict::RestrictedInstrument;
            }
            const auto notional =
                checked_notional(command.limit_price, command.quantity);
            if (verdict == RiskVerdict::Allowed && !notional.has_value()) {
                verdict = RiskVerdict::OrderNotionalExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                (command.limit_price < client->limits.minimum_price ||
                 command.limit_price > client->limits.maximum_price ||
                 command.limit_price < account->limits.minimum_price ||
                 command.limit_price > account->limits.maximum_price)) {
                verdict = RiskVerdict::PriceOutsideCollar;
            }
            if (verdict == RiskVerdict::Allowed &&
                (command.quantity >
                     client->limits.maximum_single_order_quantity ||
                 command.quantity >
                     account->limits.maximum_single_order_quantity)) {
                verdict = RiskVerdict::OrderQuantityExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                (*notional > client->limits.maximum_single_order_notional ||
                 *notional > account->limits.maximum_single_order_notional)) {
                verdict = RiskVerdict::OrderNotionalExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                command.quantity.units >
                    client->limits.maximum_open_quantity.units -
                        client->open_quantity.units) {
                verdict = RiskVerdict::ClientOpenQuantityExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                notional->units >
                    client->limits.maximum_open_notional.units -
                        client->open_notional.units) {
                verdict = RiskVerdict::ClientOpenNotionalExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                client->open_orders >= client->limits.maximum_open_orders) {
                verdict = RiskVerdict::ClientOpenOrderLimitExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                command.quantity.units >
                    account->limits.maximum_open_quantity.units -
                        account->open_quantity.units) {
                verdict = RiskVerdict::AccountOpenQuantityExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                notional->units >
                    account->limits.maximum_open_notional.units -
                        account->open_notional.units) {
                verdict = RiskVerdict::AccountOpenNotionalExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                account->open_orders >= account->limits.maximum_open_orders) {
                verdict = RiskVerdict::AccountOpenOrderLimitExceeded;
            }
            const Probe parent = probe_parent(command.parent_order_id);
            if (verdict == RiskVerdict::Allowed && parent.found) {
                verdict = RiskVerdict::DuplicateParent;
            } else if (verdict == RiskVerdict::Allowed &&
                       !parent.insertable) {
                verdict = RiskVerdict::ActiveParentCapacityExceeded;
            }
            if (verdict == RiskVerdict::Allowed &&
                !normal_token_available) {
                verdict = RiskVerdict::NormalRateLimited;
            }
            if (verdict == RiskVerdict::Allowed) {
                delta.reservation_quantity = command.quantity;
                delta.reservation_notional = *notional;
            }
        } else if (command.kind == RiskCommandKind::Cancel) {
            if (verdict == RiskVerdict::Allowed &&
                (command.quantity || command.limit_price.units != 0 ||
                 !command.instrument)) {
                verdict = RiskVerdict::InvalidCommand;
            }
            const ParentSlot* const parent =
                find_parent(command.parent_order_id);
            if (verdict == RiskVerdict::Allowed && parent == nullptr) {
                verdict = RiskVerdict::ParentNotFound;
            } else if (verdict == RiskVerdict::Allowed &&
                       (parent->client_id != command.client_id ||
                        parent->account_id != command.account_id ||
                        parent->instrument != command.instrument)) {
                verdict = RiskVerdict::ParentOwnershipMismatch;
            }
            if (verdict == RiskVerdict::Allowed &&
                !cancel_token_available) {
                verdict = RiskVerdict::CancelRateLimited;
            }
        } else {
            verdict = RiskVerdict::InvalidCommand;
        }

        return finish_plan(delta, RiskPlanDisposition::Planned, verdict, true);
    }

    [[nodiscard]] bool next_generation_available() const noexcept {
        return state_generation != std::numeric_limits<std::uint64_t>::max();
    }

    [[nodiscard]] RiskAccountingStatus account_quantity(
        const ParentOrderId parent_id,
        const Quantity quantity) noexcept {
        if (!quantity) {
            return RiskAccountingStatus::InvalidQuantity;
        }
        const Probe location = probe_parent(parent_id);
        if (!location.found) {
            return RiskAccountingStatus::ParentNotFound;
        }
        ParentSlot& parent = parents[location.index];
        if (quantity > parent.remaining_quantity) {
            return RiskAccountingStatus::QuantityExceedsReservation;
        }
        if (!next_generation_available()) {
            return RiskAccountingStatus::GenerationExhausted;
        }
        ClientSlot* const client = find_client(parent.client_id);
        AccountSlot* const account = find_account(parent.account_id);
        const auto released_notional =
            checked_notional(parent.limit_price, quantity);
        if (client == nullptr || account == nullptr ||
            !released_notional.has_value() ||
            client->open_quantity < quantity ||
            account->open_quantity < quantity ||
            client->open_notional < *released_notional ||
            account->open_notional < *released_notional ||
            parent.remaining_notional < *released_notional ||
            client->open_orders == 0U || account->open_orders == 0U) {
            return RiskAccountingStatus::InvariantViolation;
        }
        client->open_quantity.units -= quantity.units;
        client->open_notional.units -= released_notional->units;
        account->open_quantity.units -= quantity.units;
        account->open_notional.units -= released_notional->units;
        parent.remaining_quantity.units -= quantity.units;
        parent.remaining_notional.units -= released_notional->units;
        if (parent.remaining_quantity.units == 0U) {
            if (parent.remaining_notional.units != 0U) {
                return RiskAccountingStatus::InvariantViolation;
            }
            --client->open_orders;
            --account->open_orders;
            parent.state = SlotState::Tombstone;
            --active_parent_count;
        }
        ++state_generation;
        return RiskAccountingStatus::Applied;
    }

    RiskKernelConfig config;
    std::unique_ptr<AccountSlot[]> accounts;
    std::unique_ptr<ClientSlot[]> clients;
    std::unique_ptr<ParentSlot[]> parents;
    std::unique_ptr<DedupeSlot[]> dedupe;
    std::unique_ptr<RestrictedSlot[]> restricted;
    std::size_t configured_account_count{};
    std::size_t configured_client_count{};
    std::uint64_t state_generation{1U};
    std::uint64_t last_committed_time_ns{};
    std::size_t active_parent_count{};
    std::size_t dedupe_count{};
    std::size_t dedupe_tombstones{};
    bool kill_switch{};
    bool recovery_only{};
};

RiskKernel::RiskKernel(
    const RiskKernelConfig config,
    const std::span<const AccountRiskConfig> accounts,
    const std::span<const ClientRiskConfig> clients,
    const std::span<const InstrumentId> restricted_instruments) {
    if (config.risk_generation == 0U ||
        config.configuration_generation == 0U ||
        (config.delta_authentication_key[0] == 0U &&
         config.delta_authentication_key[1] == 0U) ||
        config.client_capacity == 0U || config.account_capacity == 0U ||
        config.active_parent_capacity == 0U ||
        config.dedupe_capacity == 0U ||
        clients.size() > config.client_capacity ||
        accounts.size() > config.account_capacity ||
        restricted_instruments.size() >
            config.restricted_instrument_capacity) {
        throw std::invalid_argument("invalid risk kernel capacity or generation");
    }
    for (const AccountRiskConfig& account : accounts) {
        if (account.account_id == 0U ||
            !valid_permissions(account.permissions) ||
            !limits_valid(account.limits)) {
            throw std::invalid_argument("invalid account risk configuration");
        }
    }
    for (const ClientRiskConfig& client : clients) {
        if (client.client_id == 0U || client.account_id == 0U ||
            !valid_permissions(client.permissions) ||
            !limits_valid(client.limits) ||
            !bucket_valid(client.normal_rate) ||
            !bucket_valid(client.cancel_rate)) {
            throw std::invalid_argument("invalid client risk configuration");
        }
    }
    for (const InstrumentId instrument : restricted_instruments) {
        if (!instrument) {
            throw std::invalid_argument("invalid restricted instrument");
        }
    }
    impl_ = std::make_unique<Impl>(config, accounts, clients,
                                   restricted_instruments);
}

RiskKernel::~RiskKernel() = default;
RiskKernel::RiskKernel(RiskKernel&&) noexcept = default;
RiskKernel& RiskKernel::operator=(RiskKernel&&) noexcept = default;

RiskPlanResult RiskKernel::plan(
    const RiskCommand& command,
    const std::uint64_t monotonic_time_ns) const noexcept {
    if (impl_->recovery_only) {
        RiskPlanResult result{};
        result.disposition = RiskPlanDisposition::RecoveryOnly;
        result.verdict = RiskVerdict::RecoveryOnly;
        result.delta.disposition = result.disposition;
        result.delta.verdict = result.verdict;
        return result;
    }
    return impl_->plan(command, monotonic_time_ns);
}

RiskCommitStatus RiskKernel::commit(const RiskDelta& delta) noexcept {
    if (impl_->recovery_only) {
        return RiskCommitStatus::RecoveryOnly;
    }
    return commit_impl(delta);
}

RiskCommitStatus RiskKernel::commit_impl(const RiskDelta& delta) noexcept {
    if (delta.disposition == RiskPlanDisposition::DuplicateReplay) {
        return RiskCommitStatus::ReplayNoop;
    }
    if (!delta.committable()) {
        return RiskCommitStatus::NotCommittable;
    }
    if (delta.risk_generation != impl_->config.risk_generation) {
        return RiskCommitStatus::RiskGenerationMismatch;
    }
    if (delta.configuration_generation !=
        impl_->config.configuration_generation) {
        return RiskCommitStatus::ConfigurationGenerationMismatch;
    }
    if (delta.kernel_state_generation != impl_->state_generation ||
        delta.monotonic_time_ns < impl_->last_committed_time_ns) {
        return RiskCommitStatus::StaleDelta;
    }
    if (delta.command_fingerprint !=
            command_fingerprint(delta.command) ||
        delta.authentication_tag != impl_->authenticate(delta)) {
        return RiskCommitStatus::ForgedDelta;
    }
    if (!impl_->next_generation_available()) {
        return RiskCommitStatus::GenerationExhausted;
    }
    const Impl::Probe dedupe_location = impl_->probe_dedupe(
        delta.command.client_id, delta.command.request_id);
    if (dedupe_location.found) {
        const Impl::DedupeSlot& prior = impl_->dedupe[dedupe_location.index];
        return prior.fingerprint == delta.command_fingerprint
                   ? RiskCommitStatus::ReplayNoop
                   : RiskCommitStatus::DedupeConflict;
    }
    if (!dedupe_location.insertable) {
        return RiskCommitStatus::CapacityExceeded;
    }

    Impl::ClientSlot* const client =
        impl_->find_client(delta.command.client_id);
    Impl::AccountSlot* const account =
        impl_->find_account(delta.command.account_id);
    Impl::Probe parent_location{};
    if (delta.verdict == RiskVerdict::Allowed &&
        delta.command.kind == RiskCommandKind::NewOrder) {
        if (client == nullptr || account == nullptr ||
            client->account_id != account->id) {
            return RiskCommitStatus::InvariantViolation;
        }
        parent_location = impl_->probe_parent(delta.command.parent_order_id);
        if (parent_location.found || !parent_location.insertable) {
            return RiskCommitStatus::CapacityExceeded;
        }
        if (delta.reservation_quantity.units >
                client->limits.maximum_open_quantity.units -
                    client->open_quantity.units ||
            delta.reservation_notional.units >
                client->limits.maximum_open_notional.units -
                    client->open_notional.units ||
            delta.reservation_quantity.units >
                account->limits.maximum_open_quantity.units -
                    account->open_quantity.units ||
            delta.reservation_notional.units >
                account->limits.maximum_open_notional.units -
                    account->open_notional.units ||
            client->open_orders >= client->limits.maximum_open_orders ||
            account->open_orders >= account->limits.maximum_open_orders) {
            return RiskCommitStatus::InvariantViolation;
        }
    }
    if (client != nullptr) {
        if (delta.normal_tokens_after > client->normal.config.capacity ||
            delta.cancel_tokens_after > client->cancel.config.capacity ||
            delta.normal_refill_time_after > delta.monotonic_time_ns ||
            delta.cancel_refill_time_after > delta.monotonic_time_ns) {
            return RiskCommitStatus::InvariantViolation;
        }
    }

    if (client != nullptr) {
        client->normal.tokens = delta.normal_tokens_after;
        client->normal.refill_time_ns = delta.normal_refill_time_after;
        client->cancel.tokens = delta.cancel_tokens_after;
        client->cancel.refill_time_ns = delta.cancel_refill_time_after;
    }
    if (delta.verdict == RiskVerdict::Allowed &&
        delta.command.kind == RiskCommandKind::NewOrder) {
        client->open_quantity.units += delta.reservation_quantity.units;
        client->open_notional.units += delta.reservation_notional.units;
        ++client->open_orders;
        account->open_quantity.units += delta.reservation_quantity.units;
        account->open_notional.units += delta.reservation_notional.units;
        ++account->open_orders;
        impl_->parents[parent_location.index] = Impl::ParentSlot{
            SlotState::Occupied,
            delta.command.parent_order_id,
            delta.command.client_id,
            delta.command.account_id,
            delta.command.instrument,
            delta.reservation_quantity,
            delta.reservation_notional,
            delta.command.limit_price};
        ++impl_->active_parent_count;
    }

    Impl::DedupeSlot& dedupe = impl_->dedupe[dedupe_location.index];
    if (dedupe.state == SlotState::Tombstone) {
        --impl_->dedupe_tombstones;
    }
    dedupe = Impl::DedupeSlot{SlotState::Occupied,
                              delta.command.client_id,
                              delta.command.request_id,
                              delta.command_fingerprint,
                              delta.verdict,
                              delta.reservation_quantity,
                              delta.reservation_notional};
    ++impl_->dedupe_count;
    impl_->last_committed_time_ns = delta.monotonic_time_ns;
    ++impl_->state_generation;
    return RiskCommitStatus::Committed;
}

RiskAccountingStatus RiskKernel::release(
    const ParentOrderId parent,
    const Quantity quantity) noexcept {
    if (impl_->recovery_only) {
        return RiskAccountingStatus::RecoveryOnly;
    }
    return impl_->account_quantity(parent, quantity);
}

RiskAccountingStatus RiskKernel::fill(
    const ParentOrderId parent,
    const Quantity quantity) noexcept {
    if (impl_->recovery_only) {
        return RiskAccountingStatus::RecoveryOnly;
    }
    return impl_->account_quantity(parent, quantity);
}

RiskAccountingStatus RiskKernel::terminal(
    const ParentOrderId parent) noexcept {
    if (impl_->recovery_only) {
        return RiskAccountingStatus::RecoveryOnly;
    }
    const Impl::ParentSlot* const found = impl_->find_parent(parent);
    if (found == nullptr) {
        return RiskAccountingStatus::ParentNotFound;
    }
    return impl_->account_quantity(parent, found->remaining_quantity);
}

bool RiskKernel::begin_recovery() noexcept {
    if (impl_->recovery_only || impl_->state_generation != 1U ||
        impl_->last_committed_time_ns != 0U ||
        impl_->active_parent_count != 0U || impl_->dedupe_count != 0U ||
        impl_->dedupe_tombstones != 0U || impl_->kill_switch) {
        return false;
    }
    impl_->recovery_only = true;
    return true;
}

RiskRecoveryImportStatus RiskKernel::import_recovery_snapshot(
    const RiskRecoveryKernelState& state,
    const std::span<const RecoveredAccountRiskState> account_states,
    const std::span<const RecoveredClientRiskState> client_states,
    const std::span<const RecoveredParentRiskState> parent_states,
    const std::span<const RecoveredDedupeRiskState> dedupe_states) noexcept {
    if (!impl_->recovery_only) {
        return RiskRecoveryImportStatus::NotRecoveryOnly;
    }
    if (state.risk_generation != impl_->config.risk_generation ||
        state.configuration_generation !=
            impl_->config.configuration_generation) {
        return RiskRecoveryImportStatus::ConfigurationMismatch;
    }
    if (state.kernel_state_generation == 0U) {
        return RiskRecoveryImportStatus::InvalidState;
    }
    if (account_states.size() != impl_->configured_account_count ||
        client_states.size() != impl_->configured_client_count) {
        return RiskRecoveryImportStatus::IdentityMismatch;
    }
    if (parent_states.size() > impl_->config.active_parent_capacity ||
        dedupe_states.size() > impl_->config.dedupe_capacity) {
        return RiskRecoveryImportStatus::CapacityExceeded;
    }

    for (std::size_t index = 0U; index < account_states.size(); ++index) {
        const RecoveredAccountRiskState& recovered = account_states[index];
        const Impl::AccountSlot* const configured =
            impl_->find_account(recovered.account_id);
        if (configured == nullptr) {
            return RiskRecoveryImportStatus::IdentityMismatch;
        }
        if (recovered.open_quantity >
                configured->limits.maximum_open_quantity ||
            recovered.open_notional >
                configured->limits.maximum_open_notional ||
            recovered.open_orders > configured->limits.maximum_open_orders) {
            return RiskRecoveryImportStatus::CounterMismatch;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (account_states[prior].account_id == recovered.account_id) {
                return RiskRecoveryImportStatus::DuplicateIdentity;
            }
        }
    }

    for (std::size_t index = 0U; index < client_states.size(); ++index) {
        const RecoveredClientRiskState& recovered = client_states[index];
        const Impl::ClientSlot* const configured =
            impl_->find_client(recovered.client_id);
        if (configured == nullptr ||
            configured->account_id != recovered.account_id) {
            return RiskRecoveryImportStatus::IdentityMismatch;
        }
        if (recovered.open_quantity >
                configured->limits.maximum_open_quantity ||
            recovered.open_notional >
                configured->limits.maximum_open_notional ||
            recovered.open_orders > configured->limits.maximum_open_orders) {
            return RiskRecoveryImportStatus::CounterMismatch;
        }
        if (recovered.normal_tokens > configured->normal.config.capacity ||
            recovered.cancel_tokens > configured->cancel.config.capacity ||
            recovered.normal_refill_time_ns >
                state.last_committed_monotonic_time_ns ||
            recovered.cancel_refill_time_ns >
                state.last_committed_monotonic_time_ns) {
            return RiskRecoveryImportStatus::TokenStateInvalid;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (client_states[prior].client_id == recovered.client_id) {
                return RiskRecoveryImportStatus::DuplicateIdentity;
            }
        }
    }

    for (std::size_t index = 0U; index < parent_states.size(); ++index) {
        const RecoveredParentRiskState& recovered = parent_states[index];
        const Impl::ClientSlot* const client =
            impl_->find_client(recovered.client_id);
        const Impl::AccountSlot* const account =
            impl_->find_account(recovered.account_id);
        const auto expected_notional = checked_notional(
            recovered.limit_price, recovered.remaining_quantity);
        if (!recovered.parent_order_id || recovered.client_id == 0U ||
            recovered.account_id == 0U || !recovered.instrument ||
            !recovered.remaining_quantity ||
            recovered.limit_price.units <= 0 || client == nullptr ||
            account == nullptr || client->account_id != recovered.account_id ||
            !expected_notional.has_value() ||
            *expected_notional != recovered.remaining_notional) {
            return RiskRecoveryImportStatus::InvariantViolation;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (parent_states[prior].parent_order_id ==
                recovered.parent_order_id) {
                return RiskRecoveryImportStatus::DuplicateIdentity;
            }
        }
    }

    // Active parents are the source of truth for aggregate reservations. Scan
    // them against every configured scope before mutating a slot.
    for (const RecoveredClientRiskState& client : client_states) {
        std::uint64_t quantity{};
        std::uint64_t notional{};
        std::uint32_t orders{};
        for (const RecoveredParentRiskState& parent : parent_states) {
            if (parent.client_id != client.client_id) {
                continue;
            }
            if (parent.remaining_quantity.units >
                    std::numeric_limits<std::uint64_t>::max() - quantity ||
                parent.remaining_notional.units >
                    std::numeric_limits<std::uint64_t>::max() - notional ||
                orders == std::numeric_limits<std::uint32_t>::max()) {
                return RiskRecoveryImportStatus::CounterMismatch;
            }
            quantity += parent.remaining_quantity.units;
            notional += parent.remaining_notional.units;
            ++orders;
        }
        if (quantity != client.open_quantity.units ||
            notional != client.open_notional.units ||
            orders != client.open_orders) {
            return RiskRecoveryImportStatus::CounterMismatch;
        }
    }
    for (const RecoveredAccountRiskState& account : account_states) {
        std::uint64_t quantity{};
        std::uint64_t notional{};
        std::uint32_t orders{};
        for (const RecoveredParentRiskState& parent : parent_states) {
            if (parent.account_id != account.account_id) {
                continue;
            }
            if (parent.remaining_quantity.units >
                    std::numeric_limits<std::uint64_t>::max() - quantity ||
                parent.remaining_notional.units >
                    std::numeric_limits<std::uint64_t>::max() - notional ||
                orders == std::numeric_limits<std::uint32_t>::max()) {
                return RiskRecoveryImportStatus::CounterMismatch;
            }
            quantity += parent.remaining_quantity.units;
            notional += parent.remaining_notional.units;
            ++orders;
        }
        if (quantity != account.open_quantity.units ||
            notional != account.open_notional.units ||
            orders != account.open_orders) {
            return RiskRecoveryImportStatus::CounterMismatch;
        }
    }

    for (std::size_t index = 0U; index < dedupe_states.size(); ++index) {
        const RecoveredDedupeRiskState& recovered = dedupe_states[index];
        if (recovered.client_id == 0U || recovered.request_id == 0U ||
            recovered.command_fingerprint == 0U ||
            !valid_verdict(recovered.verdict) ||
            recovered.verdict == RiskVerdict::RecoveryOnly ||
            ((recovered.reservation_quantity.units == 0U) !=
             (recovered.reservation_notional.units == 0U)) ||
            (recovered.verdict != RiskVerdict::Allowed &&
             (recovered.reservation_quantity.units != 0U ||
              recovered.reservation_notional.units != 0U))) {
            return RiskRecoveryImportStatus::InvariantViolation;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (dedupe_states[prior].client_id == recovered.client_id &&
                dedupe_states[prior].request_id == recovered.request_id) {
                return RiskRecoveryImportStatus::DuplicateIdentity;
            }
        }
    }

    // All validation has completed. From here to return there are no fallible
    // operations, so a rejected snapshot can never partially alter live state.
    for (std::size_t index = 0U; index < impl_->config.account_capacity;
         ++index) {
        Impl::AccountSlot& account = impl_->accounts[index];
        if (account.state == SlotState::Occupied) {
            account.open_quantity = Quantity{};
            account.open_notional = Notional{};
            account.open_orders = 0U;
        }
    }
    for (std::size_t index = 0U; index < impl_->config.client_capacity;
         ++index) {
        Impl::ClientSlot& client = impl_->clients[index];
        if (client.state == SlotState::Occupied) {
            client.open_quantity = Quantity{};
            client.open_notional = Notional{};
            client.open_orders = 0U;
            client.normal.tokens = client.normal.config.capacity;
            client.normal.refill_time_ns = 0U;
            client.cancel.tokens = client.cancel.config.capacity;
            client.cancel.refill_time_ns = 0U;
        }
    }
    for (std::size_t index = 0U;
         index < impl_->config.active_parent_capacity; ++index) {
        impl_->parents[index] = Impl::ParentSlot{};
    }
    for (std::size_t index = 0U; index < impl_->config.dedupe_capacity;
         ++index) {
        impl_->dedupe[index] = Impl::DedupeSlot{};
    }

    for (const RecoveredAccountRiskState& recovered : account_states) {
        Impl::AccountSlot* const account =
            impl_->find_account(recovered.account_id);
        account->open_quantity = recovered.open_quantity;
        account->open_notional = recovered.open_notional;
        account->open_orders = recovered.open_orders;
    }
    for (const RecoveredClientRiskState& recovered : client_states) {
        Impl::ClientSlot* const client = impl_->find_client(recovered.client_id);
        client->open_quantity = recovered.open_quantity;
        client->open_notional = recovered.open_notional;
        client->open_orders = recovered.open_orders;
        client->normal.tokens = recovered.normal_tokens;
        client->cancel.tokens = recovered.cancel_tokens;
        client->normal.refill_time_ns = recovered.normal_refill_time_ns;
        client->cancel.refill_time_ns = recovered.cancel_refill_time_ns;
    }
    for (const RecoveredParentRiskState& recovered : parent_states) {
        const Impl::Probe location =
            impl_->probe_parent(recovered.parent_order_id);
        impl_->parents[location.index] = Impl::ParentSlot{
            SlotState::Occupied,
            recovered.parent_order_id,
            recovered.client_id,
            recovered.account_id,
            recovered.instrument,
            recovered.remaining_quantity,
            recovered.remaining_notional,
            recovered.limit_price};
    }
    for (const RecoveredDedupeRiskState& recovered : dedupe_states) {
        const Impl::Probe location =
            impl_->probe_dedupe(recovered.client_id, recovered.request_id);
        impl_->dedupe[location.index] = Impl::DedupeSlot{
            SlotState::Occupied,
            recovered.client_id,
            recovered.request_id,
            recovered.command_fingerprint,
            recovered.verdict,
            recovered.reservation_quantity,
            recovered.reservation_notional};
    }
    impl_->active_parent_count = parent_states.size();
    impl_->dedupe_count = dedupe_states.size();
    impl_->dedupe_tombstones = 0U;
    impl_->state_generation = state.kernel_state_generation;
    impl_->last_committed_time_ns =
        state.last_committed_monotonic_time_ns;
    impl_->kill_switch = state.kill_switch;
    return RiskRecoveryImportStatus::Imported;
}

RiskRecoveryImportStatus RiskKernel::restore_recovered_delta(
    const RiskDelta& delta) noexcept {
    if (!impl_->recovery_only) {
        return RiskRecoveryImportStatus::NotRecoveryOnly;
    }
    if (!delta.committable()) {
        return RiskRecoveryImportStatus::InvalidState;
    }
    if (delta.risk_generation != impl_->config.risk_generation ||
        delta.configuration_generation !=
            impl_->config.configuration_generation) {
        return RiskRecoveryImportStatus::ConfigurationMismatch;
    }
    if (delta.command_fingerprint != command_fingerprint(delta.command) ||
        delta.authentication_tag != impl_->authenticate(delta)) {
        return RiskRecoveryImportStatus::ForgedDelta;
    }
    const Impl::Probe dedupe = impl_->probe_dedupe(
        delta.command.client_id, delta.command.request_id);
    if (dedupe.found) {
        return impl_->dedupe[dedupe.index].fingerprint ==
                       delta.command_fingerprint
                   ? RiskRecoveryImportStatus::AlreadyImported
                   : RiskRecoveryImportStatus::DedupeConflict;
    }
    const RiskCommitStatus status = commit_impl(delta);
    switch (status) {
        case RiskCommitStatus::Committed:
            return RiskRecoveryImportStatus::Imported;
        case RiskCommitStatus::ReplayNoop:
            return RiskRecoveryImportStatus::AlreadyImported;
        case RiskCommitStatus::RiskGenerationMismatch:
        case RiskCommitStatus::ConfigurationGenerationMismatch:
            return RiskRecoveryImportStatus::ConfigurationMismatch;
        case RiskCommitStatus::StaleDelta:
            return RiskRecoveryImportStatus::StateGenerationMismatch;
        case RiskCommitStatus::DedupeConflict:
            return RiskRecoveryImportStatus::DedupeConflict;
        case RiskCommitStatus::CapacityExceeded:
            return RiskRecoveryImportStatus::CapacityExceeded;
        case RiskCommitStatus::ForgedDelta:
            return RiskRecoveryImportStatus::ForgedDelta;
        case RiskCommitStatus::GenerationExhausted:
            return RiskRecoveryImportStatus::GenerationExhausted;
        case RiskCommitStatus::InvariantViolation:
            return RiskRecoveryImportStatus::InvariantViolation;
        case RiskCommitStatus::NotCommittable:
        case RiskCommitStatus::RecoveryOnly:
            return RiskRecoveryImportStatus::InvalidState;
    }
    return RiskRecoveryImportStatus::InvalidState;
}

RiskAccountingStatus RiskKernel::apply_recovered_release(
    const ParentOrderId parent,
    const Quantity quantity) noexcept {
    if (!impl_->recovery_only) {
        return RiskAccountingStatus::RecoveryOnly;
    }
    return impl_->account_quantity(parent, quantity);
}

RiskAccountingStatus RiskKernel::apply_recovered_fill(
    const ParentOrderId parent,
    const Quantity quantity) noexcept {
    if (!impl_->recovery_only) {
        return RiskAccountingStatus::RecoveryOnly;
    }
    return impl_->account_quantity(parent, quantity);
}

RiskAccountingStatus RiskKernel::apply_recovered_terminal(
    const ParentOrderId parent) noexcept {
    if (!impl_->recovery_only) {
        return RiskAccountingStatus::RecoveryOnly;
    }
    const Impl::ParentSlot* const found = impl_->find_parent(parent);
    if (found == nullptr) {
        return RiskAccountingStatus::ParentNotFound;
    }
    return impl_->account_quantity(parent, found->remaining_quantity);
}

bool RiskKernel::complete_recovery() noexcept {
    if (!impl_->recovery_only) {
        return false;
    }
    impl_->recovery_only = false;
    return true;
}

bool RiskKernel::recovery_only() const noexcept {
    return impl_->recovery_only;
}

RiskRecoveryKernelState RiskKernel::recovery_kernel_state() const noexcept {
    return RiskRecoveryKernelState{
        impl_->config.risk_generation,
        impl_->config.configuration_generation,
        impl_->state_generation,
        impl_->last_committed_time_ns,
        impl_->kill_switch};
}

RiskRecoveryExportResult RiskKernel::export_recovery_snapshot(
    const std::span<RecoveredAccountRiskState> account_states,
    const std::span<RecoveredClientRiskState> client_states,
    const std::span<RecoveredParentRiskState> parent_states,
    const std::span<RecoveredDedupeRiskState> dedupe_states) const noexcept {
    RiskRecoveryExportResult result{};
    result.account_count = impl_->configured_account_count;
    result.client_count = impl_->configured_client_count;
    result.active_parent_count = impl_->active_parent_count;
    result.dedupe_count = impl_->dedupe_count;
    if (account_states.size() < result.account_count ||
        client_states.size() < result.client_count ||
        parent_states.size() < result.active_parent_count ||
        dedupe_states.size() < result.dedupe_count) {
        result.status = RiskRecoveryExportStatus::CapacityExceeded;
        return result;
    }

    std::size_t account_count{};
    for (std::size_t index = 0U; index < impl_->config.account_capacity;
         ++index) {
        const Impl::AccountSlot& account = impl_->accounts[index];
        if (account.state == SlotState::Occupied) {
            account_states[account_count++] = RecoveredAccountRiskState{
                account.id, account.open_quantity, account.open_notional,
                account.open_orders};
        }
    }
    std::size_t client_count{};
    for (std::size_t index = 0U; index < impl_->config.client_capacity;
         ++index) {
        const Impl::ClientSlot& client = impl_->clients[index];
        if (client.state == SlotState::Occupied) {
            client_states[client_count++] = RecoveredClientRiskState{
                client.id,
                client.account_id,
                client.open_quantity,
                client.open_notional,
                client.open_orders,
                client.normal.tokens,
                client.cancel.tokens,
                client.normal.refill_time_ns,
                client.cancel.refill_time_ns};
        }
    }
    std::size_t parent_count{};
    for (std::size_t index = 0U;
         index < impl_->config.active_parent_capacity; ++index) {
        const Impl::ParentSlot& parent = impl_->parents[index];
        if (parent.state == SlotState::Occupied) {
            parent_states[parent_count++] = RecoveredParentRiskState{
                parent.id,
                parent.client_id,
                parent.account_id,
                parent.instrument,
                parent.remaining_quantity,
                parent.remaining_notional,
                parent.limit_price};
        }
    }
    std::size_t dedupe_count{};
    for (std::size_t index = 0U; index < impl_->config.dedupe_capacity;
         ++index) {
        const Impl::DedupeSlot& dedupe = impl_->dedupe[index];
        if (dedupe.state == SlotState::Occupied) {
            dedupe_states[dedupe_count++] = RecoveredDedupeRiskState{
                dedupe.client_id,
                dedupe.request_id,
                dedupe.fingerprint,
                dedupe.verdict,
                dedupe.reservation_quantity,
                dedupe.reservation_notional};
        }
    }

    // Canonical ascending order makes checkpoint bytes independent of the
    // open-addressed table layout and tombstone history.
    for (std::size_t index = 1U; index < account_count; ++index) {
        std::size_t current = index;
        while (current != 0U &&
               account_states[current].account_id <
                   account_states[current - 1U].account_id) {
            std::swap(account_states[current], account_states[current - 1U]);
            --current;
        }
    }
    for (std::size_t index = 1U; index < client_count; ++index) {
        std::size_t current = index;
        while (current != 0U &&
               client_states[current].client_id <
                   client_states[current - 1U].client_id) {
            std::swap(client_states[current], client_states[current - 1U]);
            --current;
        }
    }
    for (std::size_t index = 1U; index < parent_count; ++index) {
        std::size_t current = index;
        while (current != 0U &&
               parent_states[current].parent_order_id <
                   parent_states[current - 1U].parent_order_id) {
            std::swap(parent_states[current], parent_states[current - 1U]);
            --current;
        }
    }
    for (std::size_t index = 1U; index < dedupe_count; ++index) {
        std::size_t current = index;
        while (current != 0U &&
               (dedupe_states[current].client_id <
                    dedupe_states[current - 1U].client_id ||
                (dedupe_states[current].client_id ==
                     dedupe_states[current - 1U].client_id &&
                 dedupe_states[current].request_id <
                     dedupe_states[current - 1U].request_id))) {
            std::swap(dedupe_states[current], dedupe_states[current - 1U]);
            --current;
        }
    }
    result.status = RiskRecoveryExportStatus::Exported;
    return result;
}

bool RiskKernel::set_kill_switch(const bool engaged) noexcept {
    if (impl_->recovery_only) {
        return false;
    }
    if (impl_->kill_switch == engaged) {
        return true;
    }
    if (!impl_->next_generation_available()) {
        return false;
    }
    impl_->kill_switch = engaged;
    ++impl_->state_generation;
    return true;
}

bool RiskKernel::forget_request(
    const ClientId client,
    const RequestId request) noexcept {
    if (impl_->recovery_only) {
        return false;
    }
    const Impl::Probe location = impl_->probe_dedupe(client, request);
    if (!location.found || !impl_->next_generation_available()) {
        return false;
    }
    Impl::DedupeSlot& slot = impl_->dedupe[location.index];
    slot.state = SlotState::Tombstone;
    --impl_->dedupe_count;
    ++impl_->dedupe_tombstones;
    ++impl_->state_generation;
    return true;
}

ScopeRiskCounters RiskKernel::client_counters(
    const ClientId client) const noexcept {
    const Impl::ClientSlot* const found = impl_->find_client(client);
    return found == nullptr
               ? ScopeRiskCounters{}
               : ScopeRiskCounters{true,
                                   found->open_quantity,
                                   found->open_notional,
                                   found->open_orders,
                                   found->normal.tokens,
                                   found->cancel.tokens};
}

ScopeRiskCounters RiskKernel::account_counters(
    const AccountId account) const noexcept {
    const Impl::AccountSlot* const found = impl_->find_account(account);
    return found == nullptr
               ? ScopeRiskCounters{}
               : ScopeRiskCounters{true,
                                   found->open_quantity,
                                   found->open_notional,
                                   found->open_orders,
                                   0U,
                                   0U};
}

ActiveParentRisk RiskKernel::active_parent(
    const ParentOrderId parent) const noexcept {
    const Impl::ParentSlot* const found = impl_->find_parent(parent);
    return found == nullptr
               ? ActiveParentRisk{}
               : ActiveParentRisk{true,
                                  found->client_id,
                                  found->account_id,
                                  found->instrument,
                                  found->remaining_quantity,
                                  found->remaining_notional,
                                  found->limit_price};
}

RiskKernelMetadata RiskKernel::metadata() const noexcept {
    return RiskKernelMetadata{impl_->config.risk_generation,
                              impl_->config.configuration_generation,
                              impl_->state_generation,
                              impl_->last_committed_time_ns,
                              impl_->active_parent_count,
                              impl_->dedupe_count,
                              impl_->dedupe_tombstones,
                              impl_->kill_switch,
                              impl_->recovery_only};
}

}  // namespace sorx::risk
