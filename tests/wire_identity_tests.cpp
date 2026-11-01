#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;

constexpr InstrumentId kInstrument{7U};
constexpr VenueId kVenueOne{1U};
constexpr VenueId kVenueTwo{2U};

WireProfileIdentity profile_identity(
    const VenueId venue,
    const ProfileId profile,
    std::string session) {
    return WireProfileIdentity{
        venue,
        profile,
        "MockXFix44V1",
        std::move(session),
        "FIX44-MOCKX-2026",
        "SORX-" + std::to_string(venue.value()),
        "MOCKX-" + std::to_string(venue.value()),
    };
}

RouteConfig route(
    const VenueId venue,
    const std::uint32_t priority) {
    return RouteConfig{
        venue,
        kInstrument,
        "VENUE_" + std::to_string(venue.value()),
        "ALPHA",
        "ALPHA_" + std::to_string(venue.value()),
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{100U},
        priority,
        priority,
        100U + priority,
        AdminState::Up,
    };
}

std::shared_ptr<const CandidateTable> table(
    const bool two_venues = true) {
    std::vector<RouteConfig> routes{route(kVenueOne, 0U)};
    if (two_venues) {
        routes.push_back(route(kVenueTwo, 1U));
    }
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(std::move(routes), errors));
    assert(errors.empty());
    FibCompiler compiler;
    return compiler.compile(rib);
}

CoreConfiguration core_configuration(
    const bool two_venues = true) {
    CoreConfiguration value;
    value.maximum_aggressive_venues = two_venues ? 2U : 1U;
    value.risk.generation = 31U;
    value.risk.instruments.push_back(InstrumentRiskLimit{
        kInstrument,
        true,
        Quantity{1'000U},
        Quantity{1'000U},
        Notional{1'000'000U},
        Notional{1'000'000U},
        Price{1},
        Price{10'000},
        100U,
        100U,
        Quantity{10'000U},
    });
    value.risk.venues.push_back(VenueRiskLimit{
        kVenueOne, true, Quantity{10'000U}});
    if (two_venues) {
        value.risk.venues.push_back(VenueRiskLimit{
            kVenueTwo, true, Quantity{10'000U}});
    }
    return value;
}

md::BookView book(
    const VenueId venue,
    const Price ask,
    const std::uint64_t generation) {
    md::BookView value;
    value.venue = venue;
    value.channel = ChannelId{venue.value()};
    value.profile = ProfileId{100U + venue.value()};
    value.instrument = kInstrument;
    value.feed_session_epoch = 1U;
    value.recovery_generation = 1U;
    value.view_generation = generation;
    value.last_applied_sequence = generation;
    value.next_sequence = generation + 1U;
    value.exchange_timestamp_ns = 100;
    value.local_receive_time_ns = 200U;
    value.local_publication_time_ns = 201U;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    assert(value.push_bid(md::Level{
        Price{ask.units - 1}, Quantity{100U}, 1U}));
    assert(value.push_ask(md::Level{ask, Quantity{3U}, 1U}));
    assert(value.valid());
    return value;
}

CandidateRuntimeView runtime_view(md::BookView value) {
    return CandidateRuntimeView{
        value.venue,
        value.view_generation,
        std::move(value),
        MarketDataState::Live,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        true,
        true,
        true,
    };
}

template <typename Event>
ApplyResult apply_event(
    SorCore& core,
    std::uint64_t& sequence,
    Event event) {
    return core.apply(CoreEvent{++sequence, std::move(event)});
}

void publish_ready(
    SorCore& core,
    std::uint64_t& sequence,
    const VenueId venue,
    const std::uint64_t epoch,
    const Price ask,
    const std::uint64_t generation) {
    assert(apply_event(
               core,
               sequence,
               OrderEntrySessionEvent{
                   venue, epoch, OrderEntryState::Ready})
               .command_accepted);
    assert(apply_event(
               core,
               sequence,
               RuntimeViewPublishedEvent{
                   kInstrument,
                   runtime_view(book(venue, ask, generation))})
               .command_accepted);
}

std::shared_ptr<const MockXFix44WireIdentityFactory>
two_venue_factory() {
    std::vector<MockXFix44WireIdentityBinding> bindings;
    // Deliberately reversed: the immutable factory canonicalizes bindings.
    bindings.push_back(MockXFix44WireIdentityBinding{
        profile_identity(kVenueTwo, ProfileId{302U}, "SESSION-B"),
        MockXFix44Config{2U, 0U, 32U, "SX"},
    });
    bindings.push_back(MockXFix44WireIdentityBinding{
        profile_identity(kVenueOne, ProfileId{301U}, "SESSION-A"),
        MockXFix44Config{2U, 0U, 32U, "SX"},
    });
    return std::make_shared<MockXFix44WireIdentityFactory>(
        std::move(bindings));
}

const ChildSnapshot& child_at(
    const SorSnapshot& state,
    const VenueId venue) {
    const auto found = std::find_if(
        state.children.begin(),
        state.children.end(),
        [venue](const ChildSnapshot& child) {
            return child.venue == venue;
        });
    assert(found != state.children.end());
    return *found;
}

const std::string& field(const FixMessage& message, const int tag) {
    const auto found = std::find_if(
        message.fields.begin(),
        message.fields.end(),
        [tag](const FixField& item) { return item.tag == tag; });
    if (found == message.fields.end()) {
        throw std::runtime_error("missing FIX field");
    }
    return found->value;
}

DispatchStartedEvent started(
    const ChildSnapshot& child,
    const DispatchAttemptKind kind,
    const std::uint64_t attempt,
    std::string wire) {
    return DispatchStartedEvent{
        child.id,
        kind,
        attempt,
        child.venue,
        child.session_epoch,
        std::move(wire),
    };
}

LocalDispatchResult local_result(
    const DispatchStartedEvent& event,
    const LocalDispatchState state) {
    LocalDispatchResult result;
    result.child = event.child;
    result.state = state;
    result.kind = event.kind;
    result.attempt_id = event.attempt_id;
    result.venue = event.venue;
    result.session_epoch = event.session_epoch;
    result.wire_order_id = event.wire_order_id;
    return result;
}

void profile_owned_two_venue_retry_restore_test() {
    const auto candidates = table();
    const auto factory = two_venue_factory();
    assert(factory->profiles().size() == 2U);
    assert(factory->profiles()[0].venue == kVenueOne);
    assert(factory->profiles()[1].venue == kVenueTwo);
    SorCore core(core_configuration(), candidates, factory);
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence, kVenueOne, 11U, Price{100}, 100U);
    publish_ready(core, sequence, kVenueTwo, 22U, Price{101}, 200U);

    const ParentOrder order{
        ParentOrderId{1U},
        kInstrument,
        Side::Buy,
        Quantity{6U},
        Price{101},
        RoutingIntent::AggressiveIOC,
    };
    const auto submitted = apply_event(
        core, sequence, ParentSubmitEvent{order});
    assert(submitted.command_accepted);
    assert(submitted.effects.size() == 2U);
    SorSnapshot state = core.snapshot();
    assert(state.children.size() == 2U);
    const ChildSnapshot first = child_at(state, kVenueOne);
    const ChildSnapshot second = child_at(state, kVenueTwo);
    assert(first.wire_order_id == "SXN11-1-1");
    assert(first.cancel_wire_order_id == "SXC11-1-1");
    assert(second.wire_order_id == "SXN22-1-2");
    assert(second.cancel_wire_order_id == "SXC22-1-2");
    assert(first.wire_profile ==
           profile_identity(kVenueOne, ProfileId{301U}, "SESSION-A"));
    assert(second.wire_profile ==
           profile_identity(kVenueTwo, ProfileId{302U}, "SESSION-B"));

    MockXFix44Profile first_profile{
        MockXFix44Config{2U, 0U, 32U, "SX"}};
    MockXFix44Profile second_profile{
        MockXFix44Config{2U, 0U, 32U, "SX"}};
    const auto& first_new = std::get<DispatchNewEffect>(
        submitted.effects[0]).intent;
    const auto& second_new = std::get<DispatchNewEffect>(
        submitted.effects[1]).intent;
    assert(field(first_profile.make_new_order(first_new).message, 11) ==
           first.wire_order_id);
    assert(field(second_profile.make_new_order(second_new).message, 11) ==
           second.wire_order_id);

    const std::vector<std::byte> saved = core.save_state();
    SorCore restored(core_configuration(), candidates, factory);
    restored.restore_state(saved);
    state = restored.snapshot();
    sequence = state.last_ingress_sequence;
    assert(child_at(state, kVenueOne).wire_order_id == first.wire_order_id);
    assert(child_at(state, kVenueOne).cancel_wire_order_id ==
           first.cancel_wire_order_id);
    assert(child_at(state, kVenueTwo).wire_profile == second.wire_profile);

    const auto first_start = started(
        first, DispatchAttemptKind::New, 1U, first.wire_order_id);
    assert(apply_event(restored, sequence, first_start).command_accepted);
    const auto first_unsent = apply_event(
        restored,
        sequence,
        LocalDispatchEvent{local_result(
            first_start, LocalDispatchState::DefinitelyUnsent)});
    assert(first_unsent.command_accepted);
    assert(first_unsent.effects.size() == 1U);
    assert(std::get<DispatchNewEffect>(first_unsent.effects.front())
               .intent.wire_order_id == first.wire_order_id);

    const auto first_retry = started(
        first, DispatchAttemptKind::New, 2U, first.wire_order_id);
    assert(apply_event(restored, sequence, first_retry).command_accepted);
    assert(apply_event(
               restored,
               sequence,
               LocalDispatchEvent{local_result(
                   first_retry, LocalDispatchState::AcceptedLocally)})
               .command_accepted);
    const auto second_start = started(
        second, DispatchAttemptKind::New, 3U, second.wire_order_id);
    assert(apply_event(restored, sequence, second_start).command_accepted);
    assert(apply_event(
               restored,
               sequence,
               LocalDispatchEvent{local_result(
                   second_start, LocalDispatchState::AcceptedLocally)})
               .command_accepted);

    const auto canceled = apply_event(
        restored,
        sequence,
        ParentCancelEvent{ParentOrderId{1U}});
    assert(canceled.command_accepted);
    assert(canceled.effects.size() == 2U);
    const auto& first_cancel = std::get<DispatchCancelEffect>(
        canceled.effects[0]).intent;
    const auto& second_cancel = std::get<DispatchCancelEffect>(
        canceled.effects[1]).intent;
    assert(first_cancel.cancel_wire_order_id ==
           first.cancel_wire_order_id);
    assert(second_cancel.cancel_wire_order_id ==
           second.cancel_wire_order_id);
    assert(field(first_profile.make_cancel(first_cancel).message, 11) ==
           first.cancel_wire_order_id);
    assert(field(second_profile.make_cancel(second_cancel).message, 11) ==
           second.cancel_wire_order_id);

    const auto cancel_start = started(
        first,
        DispatchAttemptKind::Cancel,
        4U,
        first.cancel_wire_order_id);
    assert(apply_event(restored, sequence, cancel_start).command_accepted);
    const auto cancel_unsent = apply_event(
        restored,
        sequence,
        LocalDispatchEvent{local_result(
            cancel_start, LocalDispatchState::DefinitelyUnsent)});
    assert(cancel_unsent.command_accepted);
    assert(cancel_unsent.effects.size() == 1U);
    assert(std::get<DispatchCancelEffect>(cancel_unsent.effects.front())
               .intent.cancel_wire_order_id ==
           first.cancel_wire_order_id);
}

class FixedWireIdentityFactory final : public WireIdentityFactory {
public:
    FixedWireIdentityFactory(
        WireProfileIdentity profile,
        std::string new_id,
        std::string cancel_id)
        : profiles_{std::move(profile)},
          new_id_(std::move(new_id)),
          cancel_id_(std::move(cancel_id)) {}

    std::span<const WireProfileIdentity> profiles()
        const noexcept override {
        return profiles_;
    }

    WireIdentityResult make(
        const WireIdentityRequest& request) const override {
        return WireIdentityResult{
            true,
            request.profile,
            request.kind == DispatchAttemptKind::New ?
                new_id_ :
                cancel_id_,
        };
    }

private:
    std::vector<WireProfileIdentity> profiles_;
    std::string new_id_;
    std::string cancel_id_;
};

class ProfileSetWireIdentityFactory final : public WireIdentityFactory {
public:
    explicit ProfileSetWireIdentityFactory(
        std::vector<WireProfileIdentity> profiles)
        : profiles_(std::move(profiles)) {}

    std::span<const WireProfileIdentity> profiles()
        const noexcept override {
        return profiles_;
    }

    WireIdentityResult make(
        const WireIdentityRequest& request) const override {
        return WireIdentityResult{false, request.profile, {}};
    }

private:
    std::vector<WireProfileIdentity> profiles_;
};

void publish_single_ready(SorCore& core, std::uint64_t& sequence) {
    publish_ready(core, sequence, kVenueOne, 5U, Price{100}, 100U);
}

ParentOrder single_order(const std::uint64_t id) {
    return ParentOrder{
        ParentOrderId{id},
        kInstrument,
        Side::Buy,
        Quantity{3U},
        Price{100},
        RoutingIntent::AggressiveIOC,
    };
}

void mockx_v1_rejects_configuration_aliasing_test() {
    const WireProfileIdentity identity = profile_identity(
        kVenueOne, ProfileId{301U}, "SESSION-A");
    const auto rejected = [&identity](MockXFix44Config config) {
        try {
            const MockXFix44WireIdentityFactory factory{
                std::vector<MockXFix44WireIdentityBinding>{
                    MockXFix44WireIdentityBinding{
                        identity, std::move(config)}}};
            static_cast<void>(factory);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    assert(rejected(MockXFix44Config{2U, 0U, 32U, "DIFFERENT"}));
    assert(rejected(MockXFix44Config{2U, 0U, 31U, "SX"}));

    bool duplicate_rejected = false;
    try {
        const MockXFix44WireIdentityBinding binding{
            identity, MockXFix44Config{2U, 0U, 32U, "SX"}};
        const MockXFix44WireIdentityFactory duplicate{
            std::vector<MockXFix44WireIdentityBinding>{
                binding, binding}};
        static_cast<void>(duplicate);
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    assert(duplicate_rejected);
}

void maximum_length_and_duplicate_fail_transactionally_test() {
    const auto candidates = table(false);
    const WireProfileIdentity identity = profile_identity(
        kVenueOne, ProfileId{301U}, "SESSION-A");
    const auto too_long = std::make_shared<FixedWireIdentityFactory>(
        identity,
        std::string(kMaximumWireOrderIdLength + 1U, 'N'),
        "VALID-CANCEL");
    SorCore failing(core_configuration(false), candidates, too_long);
    std::uint64_t sequence = 0U;
    publish_single_ready(failing, sequence);
    const auto before = failing.snapshot();
    const auto failed = apply_event(
        failing, sequence, ParentSubmitEvent{single_order(10U)});
    assert(!failed.command_accepted);
    assert(failed.command_error == CommandError::InvalidCommand);
    assert(failed.records.empty());
    assert(failed.effects.empty());
    const auto after = failing.snapshot();
    assert(after.parents.empty());
    assert(after.children.empty());
    assert(after.last_ingress_sequence == before.last_ingress_sequence + 1U);

    const auto constant = std::make_shared<FixedWireIdentityFactory>(
        identity, "CONSTANT-NEW", "CONSTANT-CANCEL");
    SorCore duplicate(core_configuration(false), candidates, constant);
    sequence = 0U;
    publish_single_ready(duplicate, sequence);
    assert(apply_event(
               duplicate,
               sequence,
               ParentSubmitEvent{single_order(20U)})
               .command_accepted);
    const SorSnapshot one_child = duplicate.snapshot();
    assert(one_child.children.size() == 1U);
    const auto rejected = apply_event(
        duplicate,
        sequence,
        ParentSubmitEvent{single_order(21U)});
    assert(!rejected.command_accepted);
    const SorSnapshot still_one = duplicate.snapshot();
    assert(still_one.children.size() == 1U);
    assert(still_one.parents.size() == 1U);
    assert(still_one.parents.front().order.id == ParentOrderId{20U});
    assert(still_one.children.front().wire_order_id == "CONSTANT-NEW");
    assert(still_one.children.front().cancel_wire_order_id ==
           "CONSTANT-CANCEL");
}

config::VenueConfiguration configured_venue(
    const VenueId venue = kVenueOne) {
    const std::string suffix = std::to_string(venue.value());
    const VenueRiskLimit venue_risk{
        venue, true, Quantity{10'000U}};
    return config::VenueConfiguration{
        venue,
        "VENUE_" + suffix,
        AdminState::Up,
        venue_risk,
        32U,
        32U,
        8U,
        8U,
        ChannelId{100U + venue.value()},
        ProfileId{201U},
        ProfileId{300U + venue.value()},
        "MockXItchV1",
        "MD-SESSION-" + suffix,
        "MockXFix44V1",
        "OE-SESSION-" + suffix,
        "FIX44-MOCKX-2026",
        "SORX-" + suffix,
        "MOCKX-" + suffix,
        std::nullopt,
    };
}

WireProfileIdentity configured_wire_profile(
    const config::VenueConfiguration& venue) {
    return WireProfileIdentity{
        venue.id,
        venue.order_entry_profile_id,
        venue.order_entry_profile,
        venue.order_entry_session_identity,
        venue.dictionary_identity,
        venue.sender_comp_id,
        venue.target_comp_id,
    };
}

config::ValidatedConfiguration validated_configuration() {
    config::ValidatedConfiguration validated;
    validated.schema_version = 1U;
    validated.account_id = "wire-identity-runtime";
    validated.ingress_capacity = 64U;
    validated.core = core_configuration(false);
    validated.instruments.push_back(config::InstrumentConfiguration{
        kInstrument,
        "ALPHA",
        validated.core.risk.instruments.front(),
    });
    validated.venues.push_back(configured_venue());
    validated.routes.push_back(route(kVenueOne, 0U));
    validated.route_critical_hash =
        config::compute_route_critical_hash(validated);
    validated.route_critical_hash_hex =
        config::sha256_hex(validated.route_critical_hash);
    return validated;
}

config::ValidatedConfiguration two_venue_validated_configuration() {
    auto validated = validated_configuration();
    validated.core.maximum_aggressive_venues = 2U;
    validated.core.risk.venues.push_back(VenueRiskLimit{
        kVenueTwo, true, Quantity{10'000U}});
    validated.venues.push_back(configured_venue(kVenueTwo));
    validated.routes.push_back(route(kVenueTwo, 1U));
    validated.route_critical_hash =
        config::compute_route_critical_hash(validated);
    validated.route_critical_hash_hex =
        config::sha256_hex(validated.route_critical_hash);
    return validated;
}

std::shared_ptr<const MockXFix44WireIdentityFactory>
runtime_wire_factory(const bool mismatched_session = false) {
    auto venue = configured_venue();
    return std::make_shared<MockXFix44WireIdentityFactory>(
        std::vector<MockXFix44WireIdentityBinding>{
            MockXFix44WireIdentityBinding{
                WireProfileIdentity{
                    venue.id,
                    venue.order_entry_profile_id,
                    venue.order_entry_profile,
                    mismatched_session ?
                        "WRONG-SESSION" :
                        venue.order_entry_session_identity,
                    venue.dictionary_identity,
                    venue.sender_comp_id,
                    venue.target_comp_id,
                },
                MockXFix44Config{2U, 0U, 32U, "SX"},
            }});
}

class TemporaryRuntimeFiles final {
public:
    TemporaryRuntimeFiles() {
        path_ = std::filesystem::temp_directory_path() /
                ("sorx-wire-identity-" +
                 std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch().count()) +
                 ".journal");
        cleanup();
    }

    ~TemporaryRuntimeFiles() { cleanup(); }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void cleanup() const noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".lock", ignored);
        std::filesystem::remove(
            path_.string() + ".checkpoint", ignored);
        std::filesystem::remove(
            path_.string() + ".checkpoint.tmp", ignored);
    }

    std::filesystem::path path_;
};

RuntimeConfiguration runtime_configuration(
    const std::filesystem::path& path,
    const bool two_venues = false) {
    RuntimeConfiguration value;
    value.validated = two_venues ?
                          two_venue_validated_configuration() :
                          validated_configuration();
    value.journal_path = path;
    value.checkpoint_interval_batches = 8U;
    return value;
}

void runtime_requires_pinned_profile_and_emits_mockx_ids_test() {
    static_assert(!std::is_constructible_v<
                  SorRuntime,
                  RuntimeConfiguration>);
    static_assert(std::is_constructible_v<
                  SorRuntime,
                  RuntimeConfiguration,
                  std::shared_ptr<const WireIdentityFactory>>);

    TemporaryRuntimeFiles mismatched_files;
    bool mismatch_rejected = false;
    try {
        SorRuntime mismatched(
            runtime_configuration(mismatched_files.path()),
            runtime_wire_factory(true));
        static_cast<void>(mismatched);
    } catch (const std::invalid_argument&) {
        mismatch_rejected = true;
    }
    assert(mismatch_rejected);

    TemporaryRuntimeFiles missing_files;
    bool missing_rejected = false;
    try {
        SorRuntime missing(
            runtime_configuration(missing_files.path()),
            std::make_shared<ProfileSetWireIdentityFactory>(
                std::vector<WireProfileIdentity>{}));
        static_cast<void>(missing);
    } catch (const std::invalid_argument&) {
        missing_rejected = true;
    }
    assert(missing_rejected);

    TemporaryRuntimeFiles extra_files;
    bool extra_rejected = false;
    try {
        const auto first = configured_venue(kVenueOne);
        const auto second = configured_venue(kVenueTwo);
        SorRuntime extra(
            runtime_configuration(extra_files.path()),
            std::make_shared<ProfileSetWireIdentityFactory>(
                std::vector<WireProfileIdentity>{
                    configured_wire_profile(first),
                    configured_wire_profile(second)}));
        static_cast<void>(extra);
    } catch (const std::invalid_argument&) {
        extra_rejected = true;
    }
    assert(extra_rejected);

    TemporaryRuntimeFiles reordered_files;
    const auto first = configured_venue(kVenueOne);
    const auto second = configured_venue(kVenueTwo);
    SorRuntime reordered(
        runtime_configuration(reordered_files.path(), true),
        std::make_shared<ProfileSetWireIdentityFactory>(
            std::vector<WireProfileIdentity>{
                configured_wire_profile(second),
                configured_wire_profile(first)}));
    assert(reordered.inspect().children.empty());

    TemporaryRuntimeFiles files;
    const auto factory = runtime_wire_factory();
    SorRuntime runtime(runtime_configuration(files.path()), factory);
    assert(runtime.publish(OrderEntrySessionEvent{
        kVenueOne, 7U, OrderEntryState::Ready}));
    assert(runtime.publish(RuntimeViewPublishedEvent{
        kInstrument,
        runtime_view(book(kVenueOne, Price{100}, 100U))}));
    assert(runtime.complete_recovery());
    assert(runtime.submit(single_order(30U)).accepted());
    const auto new_effect = runtime.poll_egress(kVenueOne);
    assert(new_effect.has_value());
    const NewOrderIntent new_intent =
        std::get<DispatchNewEffect>(*new_effect).intent;
    assert(new_intent.wire_order_id == "SXN7-1-1");
    MockXFix44Profile profile{
        MockXFix44Config{2U, 0U, 32U, "SX"}};
    const auto new_message = profile.make_new_order(new_intent);
    assert(new_message.ok);
    assert(field(new_message.message, 11) == "SXN7-1-1");

    LocalDispatchResult accepted;
    accepted.child = new_intent.child;
    accepted.state = LocalDispatchState::AcceptedLocally;
    accepted.kind = DispatchAttemptKind::New;
    accepted.attempt_id = new_intent.dispatch_attempt_id;
    accepted.venue = new_intent.venue;
    accepted.session_epoch = new_intent.session_epoch;
    accepted.wire_order_id = new_intent.wire_order_id;
    assert(runtime.publish(LocalDispatchEvent{std::move(accepted)}));
    assert(runtime.cancel(ParentOrderId{30U}).accepted());
    const auto cancel_effect = runtime.poll_egress(kVenueOne);
    assert(cancel_effect.has_value());
    const CancelOrderIntent cancel_intent =
        std::get<DispatchCancelEffect>(*cancel_effect).intent;
    assert(cancel_intent.cancel_wire_order_id == "SXC7-1-1");
    const auto cancel_message = profile.make_cancel(cancel_intent);
    assert(cancel_message.ok);
    assert(field(cancel_message.message, 11) == "SXC7-1-1");
    assert(field(cancel_message.message, 41) == "SXN7-1-1");

    const SorSnapshot state = runtime.inspect();
    assert(state.children.front().wire_profile ==
           factory->profiles().front());
    assert(state.children.front().cancel_wire_order_id == "SXC7-1-1");
}

}  // namespace

int main() {
    profile_owned_two_venue_retry_restore_test();
    mockx_v1_rejects_configuration_aliasing_test();
    maximum_length_and_duplicate_fail_transactionally_test();
    runtime_requires_pinned_profile_and_emits_mockx_ids_test();
    return 0;
}




