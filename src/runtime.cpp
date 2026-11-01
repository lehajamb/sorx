#include "sorx/runtime.hpp"

#include "sorx/runtime_queues.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace sorx {
namespace {

VenueId effect_venue(const CoreEffect& effect) {
    return std::visit(
        [](const auto& item) { return item.intent.venue; },
        effect);
}

bool is_cancel_effect(const CoreEffect& effect) noexcept {
    return std::holds_alternative<DispatchCancelEffect>(effect);
}

DispatchStartedEvent dispatch_started_event(
    const CoreEffect& effect,
    const std::uint64_t attempt_id) {
    return std::visit(
        [attempt_id](const auto& dispatch) {
            const auto& intent = dispatch.intent;
            if constexpr (
                std::is_same_v<
                    std::decay_t<decltype(dispatch)>,
                    DispatchNewEffect>) {
                return DispatchStartedEvent{
                    intent.child,
                    adapters::DispatchAttemptKind::New,
                    attempt_id,
                    intent.venue,
                    intent.session_epoch,
                    intent.wire_order_id,
                };
            } else {
                return DispatchStartedEvent{
                    intent.child,
                    adapters::DispatchAttemptKind::Cancel,
                    attempt_id,
                    intent.venue,
                    intent.session_epoch,
                    intent.cancel_wire_order_id,
                };
            }
        },
        effect);
}

void assign_dispatch_attempt(
    CoreEffect& effect,
    const std::uint64_t attempt_id) {
    std::visit(
        [attempt_id](auto& dispatch) {
            dispatch.intent.dispatch_attempt_id = attempt_id;
        },
        effect);
}

bool child_is_terminal(const ChildState state) noexcept {
    return state == ChildState::Filled ||
           state == ChildState::Canceled ||
           state == ChildState::Rejected;
}

inline constexpr persistence::RecordType runtime_state_record_type = 2'000U;
inline constexpr std::uint32_t runtime_state_magic = 0x53585253U;
inline constexpr std::uint16_t runtime_state_version = 1U;

void put_u8(std::vector<std::byte>& output, const std::uint8_t value) {
    output.push_back(static_cast<std::byte>(value));
}

void put_u16(std::vector<std::byte>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(value & 0xffU));
}

void put_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

class RuntimeStateReader final {
public:
    explicit RuntimeStateReader(const std::span<const std::byte> bytes)
        : bytes_(bytes) {}

    std::uint8_t u8() {
        require(1U);
        return std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }

    std::uint16_t u16() {
        require(2U);
        std::uint16_t value = 0U;
        for (std::size_t index = 0U; index < 2U; ++index) {
            value = static_cast<std::uint16_t>(
                (value << 8U) |
                std::to_integer<std::uint16_t>(bytes_[offset_ + index]));
        }
        offset_ += 2U;
        return value;
    }

    std::uint32_t u32() {
        require(4U);
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint32_t>(bytes_[offset_ + index]);
        }
        offset_ += 4U;
        return value;
    }

    std::uint64_t u64() {
        require(8U);
        std::uint64_t value = 0U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint64_t>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return value;
    }

    std::vector<std::byte> bytes() {
        const std::uint32_t size = u32();
        require(size);
        std::vector<std::byte> output(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return output;
    }

    void require_end() const {
        if (offset_ != bytes_.size()) {
            throw std::runtime_error("trailing bytes in runtime state image");
        }
    }

private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - offset_) {
            throw std::runtime_error("truncated runtime state image");
        }
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

struct SavedRuntimeState final {
    std::uint64_t rib_revision{};
    std::uint64_t candidate_generation{};
    std::map<VenueId, AdminState> venue_admin;
    std::vector<std::byte> core_state;
};

std::vector<std::byte> encode_runtime_state(
    const RouteRib& rib,
    const CandidateTable& candidates,
    const SorCore& core) {
    std::map<VenueId, AdminState> venue_admin;
    for (const auto& route : rib.routes()) {
        const auto [found, inserted] =
            venue_admin.emplace(route.venue, route.admin_state);
        if (!inserted && found->second != route.admin_state) {
            throw std::logic_error(
                "RIB venue has inconsistent administrative state");
        }
    }
    const auto core_state = core.save_state();
    if (venue_admin.size() > std::numeric_limits<std::uint32_t>::max() ||
        core_state.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("runtime state image is too large");
    }

    std::vector<std::byte> output;
    put_u32(output, runtime_state_magic);
    put_u16(output, runtime_state_version);
    put_u64(output, rib.revision());
    put_u64(output, candidates.generation());
    put_u32(output, static_cast<std::uint32_t>(venue_admin.size()));
    for (const auto& [venue, state] : venue_admin) {
        put_u32(output, venue.value());
        put_u8(output, static_cast<std::uint8_t>(state));
    }
    put_u32(output, static_cast<std::uint32_t>(core_state.size()));
    output.insert(output.end(), core_state.begin(), core_state.end());
    return output;
}

SavedRuntimeState decode_runtime_state(const std::span<const std::byte> bytes) {
    RuntimeStateReader reader(bytes);
    if (reader.u32() != runtime_state_magic ||
        reader.u16() != runtime_state_version) {
        throw std::runtime_error("incompatible runtime state image");
    }
    SavedRuntimeState output;
    output.rib_revision = reader.u64();
    output.candidate_generation = reader.u64();
    if (output.rib_revision == 0U || output.candidate_generation == 0U) {
        throw std::runtime_error("invalid restored RIB/FIB generation");
    }
    const std::uint32_t venue_count = reader.u32();
    for (std::uint32_t index = 0U; index < venue_count; ++index) {
        const VenueId venue{reader.u32()};
        const auto state = static_cast<AdminState>(reader.u8());
        if (!venue ||
            (state != AdminState::Up && state != AdminState::Down) ||
            !output.venue_admin.emplace(venue, state).second) {
            throw std::runtime_error(
                "invalid venue admin state in runtime image");
        }
    }
    output.core_state = reader.bytes();
    reader.require_end();
    return output;
}

persistence::JournalHeader journal_header_for(
    const config::ValidatedConfiguration& validated) {
    if (validated.schema_version == 0U || validated.account_id.empty() ||
        validated.ingress_capacity == 0U || validated.routes.empty() ||
        validated.venues.empty()) {
        throw std::invalid_argument(
            "runtime requires a complete validated configuration");
    }
    const config::Sha256Digest recomputed =
        config::compute_route_critical_hash(validated);
    const std::string recomputed_hex = config::sha256_hex(recomputed);
    if (validated.route_critical_hash != recomputed ||
        validated.route_critical_hash_hex != recomputed_hex) {
        throw std::invalid_argument(
            "validated configuration route-critical hash mismatch");
    }

    persistence::JournalHeader header;
    header.schema_version = validated.schema_version;
    header.account_context = validated.account_id;
    for (std::size_t index = 0U;
         index < header.route_critical_hash.size();
         ++index) {
        header.route_critical_hash[index] =
            static_cast<std::byte>(recomputed[index]);
    }
    return header;
}

adapters::WireProfileIdentity configured_wire_profile(
    const config::VenueConfiguration& venue) {
    return adapters::WireProfileIdentity{
        venue.id,
        venue.order_entry_profile_id,
        venue.order_entry_profile,
        venue.order_entry_session_identity,
        venue.dictionary_identity,
        venue.sender_comp_id,
        venue.target_comp_id,
    };
}

std::shared_ptr<const adapters::WireIdentityFactory>
validate_wire_identity_factory(
    const config::ValidatedConfiguration& validated,
    std::shared_ptr<const adapters::WireIdentityFactory> factory) {
    if (!factory) {
        throw std::invalid_argument(
            "runtime requires a wire identity factory");
    }

    std::map<VenueId, adapters::WireProfileIdentity> expected;
    for (const auto& venue : validated.venues) {
        const auto profile = configured_wire_profile(venue);
        if (!profile.venue || !profile.profile ||
            profile.profile_name.empty() ||
            profile.session_identity.empty() ||
            profile.dictionary_identity.empty() ||
            profile.sender_comp_id.empty() ||
            profile.target_comp_id.empty() ||
            !expected.emplace(profile.venue, profile).second) {
            throw std::invalid_argument(
                "invalid configured wire profile identity");
        }
    }

    std::map<VenueId, adapters::WireProfileIdentity> actual;
    for (const auto& profile : factory->profiles()) {
        if (!profile.venue || !profile.profile ||
            profile.profile_name.empty() ||
            profile.session_identity.empty() ||
            profile.dictionary_identity.empty() ||
            profile.sender_comp_id.empty() ||
            profile.target_comp_id.empty() ||
            !actual.emplace(profile.venue, profile).second) {
            throw std::invalid_argument(
                "invalid or duplicate factory wire profile identity");
        }
    }
    if (actual != expected) {
        throw std::invalid_argument(
            "wire identity factory does not match validated configuration");
    }
    return factory;
}

}  // namespace

struct SorRuntime::Impl final {
    using EgressQueue = runtime::PriorityEgressQueue<CoreEffect>;
    using CoordinatorWork = std::function<void()>;

    struct Processed final {
        CommandReceipt receipt;
        std::vector<ParentReport> reports;
    };

    struct HeldNormalReservation final {
        EgressQueue* queue{};
        EgressQueue::NormalReservation reservation;
        std::size_t effect_index{};
    };

    struct DeliveryTicket final {
        std::shared_future<void> completed;

        void wait() const {
            if (completed.valid()) {
                completed.get();
            }
        }
    };

    template <typename T>
    struct Reported final {
        T value;
        DeliveryTicket delivery;
    };

    struct ReportBatch final {
        std::vector<ParentReport> reports;
        std::shared_ptr<std::promise<void>> completed;
    };

    struct SinkChange final {
        IParentReportSink* sink{};
        std::shared_ptr<std::promise<void>> completed;
    };

    using ReportWork = std::variant<ReportBatch, SinkChange>;

    struct VenueIngress final {
        std::unique_ptr<runtime::CapacityGate> market_data;
        std::unique_ptr<runtime::CapacityGate> order_entry;
    };

    struct Admission final {
        runtime::CapacityGate* gate{};
        bool critical{};
        bool valid{true};
    };

    RouteRib rib;
    FibCompiler compiler;
    CoreConfiguration core_configuration;
    std::shared_ptr<const adapters::WireIdentityFactory>
        wire_identity_factory;
    std::shared_ptr<const CandidateTable> candidates;
    std::unique_ptr<SorCore> core;
    std::filesystem::path checkpoint_path;
    std::size_t checkpoint_interval_batches{};
    std::size_t batches_since_checkpoint{};
    persistence::Journal journal;
    std::map<VenueId, std::unique_ptr<EgressQueue>> egress;
    std::map<VenueId, VenueIngress> venue_ingress;
    std::unique_ptr<runtime::BoundedPriorityIngressQueue<CoordinatorWork>>
        coordinator_queue;
    std::unique_ptr<runtime::BoundedQueue<ReportWork>> report_queue;
    std::thread coordinator_thread;
    std::thread report_thread;
    std::thread::id coordinator_thread_id;
    std::atomic<bool> accepting{true};
    std::atomic<std::uint64_t> report_callback_failure_count{};
    std::uint64_t next_ingress_sequence{1U};
    std::set<ChildOrderId> recovery_unreconciled;
    bool recovery_is_pending{true};
    bool fail_stopped{};

    Impl(
        RuntimeConfiguration configuration,
        std::shared_ptr<const adapters::WireIdentityFactory>
            input_wire_identity_factory)
        : core_configuration(configuration.validated.core),
          wire_identity_factory(validate_wire_identity_factory(
              configuration.validated,
              std::move(input_wire_identity_factory))),
          checkpoint_path(
              configuration.checkpoint_path.empty()
                  ? std::filesystem::path{
                        configuration.journal_path.string() + ".checkpoint"}
                  : std::move(configuration.checkpoint_path)),
          checkpoint_interval_batches(configuration.checkpoint_interval_batches),
          journal(
              std::move(configuration.journal_path),
              journal_header_for(configuration.validated)),
          coordinator_queue(std::make_unique<
              runtime::BoundedPriorityIngressQueue<CoordinatorWork>>(
                  configuration.validated.ingress_capacity,
                  std::max<std::size_t>(
                      1U, configuration.validated.venues.size()))),
          report_queue(std::make_unique<runtime::BoundedQueue<ReportWork>>(
              configuration.validated.ingress_capacity)) {
        if (checkpoint_interval_batches == 0U) {
            throw std::invalid_argument("checkpoint interval must be positive");
        }

        for (const auto& venue : configuration.validated.venues) {
            if (!venue.id || venue.market_data_ingress_capacity == 0U ||
                venue.order_entry_ingress_capacity == 0U ||
                venue.cancel_egress_capacity == 0U ||
                venue.normal_egress_capacity == 0U ||
                egress.contains(venue.id)) {
                throw std::invalid_argument(
                    "invalid or duplicate venue runtime configuration");
            }
            egress.emplace(
                venue.id,
                std::make_unique<EgressQueue>(
                    venue.cancel_egress_capacity,
                    venue.normal_egress_capacity));
            VenueIngress ingress;
            ingress.market_data = std::make_unique<runtime::CapacityGate>(
                venue.market_data_ingress_capacity);
            ingress.order_entry = std::make_unique<runtime::CapacityGate>(
                venue.order_entry_ingress_capacity);
            venue_ingress.emplace(venue.id, std::move(ingress));
        }
        for (const auto& route : configuration.validated.routes) {
            if (!egress.contains(route.venue)) {
                throw std::invalid_argument(
                    "configured route has no VenueRuntime");
            }
        }

        const auto replay = journal.replay();
        std::uint64_t restored_record_id = 0U;
        std::optional<SavedRuntimeState> restored;
        const auto checkpoint = persistence::CheckpointStore::read(
            checkpoint_path, journal.header());
        if (checkpoint.has_value()) {
            if (checkpoint->last_record_id > replay.last_record_id ||
                checkpoint->last_batch_id > replay.last_batch_id) {
                throw persistence::IncompatibleJournal(
                    "checkpoint is ahead of the journal");
            }
            restored = decode_runtime_state(checkpoint->payload);
            restored_record_id = checkpoint->last_record_id;
        }
        for (const auto& record : replay.records) {
            if (record.record_id > restored_record_id &&
                record.type == runtime_state_record_type) {
                restored = decode_runtime_state(record.payload);
                restored_record_id = record.record_id;
            }
        }
        if (!restored.has_value() && !replay.records.empty()) {
            throw persistence::IncompatibleJournal(
                "journal has no complete runtime state image");
        }

        auto routes = std::move(configuration.validated.routes);
        report_thread = std::thread([this] { report_loop(); });
        coordinator_thread = std::thread([this] { coordinator_loop(); });
        try {
            invoke_wait(
                [this,
                 routes = std::move(routes),
                 restored = std::move(restored)]() mutable {
                    initialize_on_coordinator(
                        std::move(routes), std::move(restored));
                },
                true);
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Impl() {
        shutdown();
    }

    void require_coordinator() const {
        if (std::this_thread::get_id() != coordinator_thread_id) {
            throw std::logic_error(
                "SorCore state accessed outside the coordinator thread");
        }
    }

    void coordinator_loop() noexcept {
        coordinator_thread_id = std::this_thread::get_id();
        while (auto work = coordinator_queue->wait_pop()) {
            (*work)();
        }
    }

    void report_loop() noexcept {
        IParentReportSink* sink = nullptr;
        while (auto work = report_queue->wait_pop()) {
            std::visit(
                [this, &sink](auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, SinkChange>) {
                        sink = item.sink;
                        item.completed->set_value();
                    } else {
                        if (sink != nullptr) {
                            for (const auto& report : item.reports) {
                                try {
                                    sink->on_parent_report(report);
                                } catch (...) {
                                    report_callback_failure_count.fetch_add(
                                        1U, std::memory_order_relaxed);
                                }
                            }
                        }
                        item.completed->set_value();
                    }
                },
                *work);
        }
    }

    void shutdown() noexcept {
        if (!accepting.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        for (auto& [venue, ingress] : venue_ingress) {
            static_cast<void>(venue);
            ingress.market_data->close();
            ingress.order_entry->close();
        }
        coordinator_queue->close();
        if (coordinator_thread.joinable()) {
            coordinator_thread.join();
        }
        report_queue->close();
        if (report_thread.joinable()) {
            report_thread.join();
        }
    }

    template <typename Function>
    auto invoke_wait(Function function, const bool critical = false)
        -> std::invoke_result_t<Function> {
        using Result = std::invoke_result_t<Function>;
        auto operation =
            std::make_shared<std::decay_t<Function>>(std::move(function));
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        CoordinatorWork work = [operation, promise]() mutable {
            try {
                if constexpr (std::is_void_v<Result>) {
                    std::invoke(*operation);
                    promise->set_value();
                } else {
                    promise->set_value(std::invoke(*operation));
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        };
        const bool queued = critical
                                ? coordinator_queue->wait_push_critical(
                                      std::move(work))
                                : coordinator_queue->wait_push_normal(
                                      std::move(work));
        if (!queued) {
            throw std::runtime_error("runtime coordinator is stopped");
        }
        if constexpr (std::is_void_v<Result>) {
            future.get();
        } else {
            return future.get();
        }
    }

    template <typename Function>
    auto invoke_try(Function function)
        -> std::optional<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        static_assert(!std::is_void_v<Result>);
        if (!accepting.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        auto operation =
            std::make_shared<std::decay_t<Function>>(std::move(function));
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        CoordinatorWork work = [operation, promise]() mutable {
            try {
                promise->set_value(std::invoke(*operation));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        };
        if (!coordinator_queue->try_push_normal(std::move(work))) {
            return std::nullopt;
        }
        return future.get();
    }

    void initialize_on_coordinator(
        std::vector<RouteConfig> routes,
        std::optional<SavedRuntimeState> restored) {
        require_coordinator();
        std::vector<RouteValidationError> errors;
        if (!rib.replace_all(std::move(routes), errors)) {
            throw std::invalid_argument("runtime RouteRib validation failed");
        }

        if (restored.has_value()) {
            for (const auto& [venue, state] : restored->venue_admin) {
                if (!rib.set_admin(venue, state)) {
                    throw persistence::IncompatibleJournal(
                        "restored venue is absent from current RIB");
                }
            }
            if (!rib.restore_revision(restored->rib_revision)) {
                throw persistence::IncompatibleJournal(
                    "restored RIB revision is invalid");
            }
            candidates = compiler.compile_at_generation(
                rib, restored->candidate_generation);
            core = std::make_unique<SorCore>(
                core_configuration,
                candidates,
                wire_identity_factory);
            core->restore_state(restored->core_state);
            next_ingress_sequence =
                core->snapshot().last_ingress_sequence + 1U;
            core->begin_recovery();

            const SorSnapshot restored_snapshot = core->snapshot();
            for (const auto& child : restored_snapshot.children) {
                if (child_is_terminal(child.state)) {
                    continue;
                }
                recovery_unreconciled.insert(child.id);
                if (child.active_dispatch_attempt.has_value() ||
                    child.state == ChildState::DispatchStarted ||
                    child.state == ChildState::PendingAcknowledgement) {
                    const Processed uncertain = process_on_coordinator(
                        DispatchTimeoutEvent{
                            child.id,
                            "recovered dispatch without a definitive venue outcome",
                        },
                        false);
                    if (!uncertain.receipt.accepted()) {
                        throw persistence::IncompatibleJournal(
                            "could not suspend uncertain recovered child");
                    }
                }
            }
        } else {
            candidates = compiler.compile(rib);
            core = std::make_unique<SorCore>(
                core_configuration,
                candidates,
                wire_identity_factory);
            core->begin_recovery();
        }

        const Processed gate =
            process_on_coordinator(RoutingGateEvent{false}, false);
        if (!gate.receipt.accepted()) {
            throw persistence::JournalIoError(
                "could not persist startup routing gate");
        }
    }

    std::vector<persistence::RecordInput> journal_inputs(
        const std::vector<DeterministicRecord>& records) const {
        std::vector<persistence::RecordInput> output;
        output.reserve(records.size());
        for (const auto& record : records) {
            output.push_back(persistence::RecordInput{
                static_cast<persistence::RecordType>(record.kind),
                1U,
                record.ingress_sequence,
                record.payload,
            });
        }
        return output;
    }

    void rollback_core(
        const std::vector<std::byte>& state,
        std::shared_ptr<const CandidateTable> previous_candidates,
        const std::uint64_t sequence) {
        require_coordinator();
        candidates = std::move(previous_candidates);
        core = std::make_unique<SorCore>(
                core_configuration,
                candidates,
                wire_identity_factory);
        core->restore_state(state);
        next_ingress_sequence = sequence;
    }

    Processed process_on_coordinator(
        CoreEventPayload payload,
        const bool public_command) {
        require_coordinator();
        Processed processed;
        if (fail_stopped) {
            processed.receipt.error = CommandError::JournalFailure;
            return processed;
        }
        if (public_command && recovery_is_pending &&
            !std::holds_alternative<ParentCancelEvent>(payload)) {
            processed.receipt.error = CommandError::RecoveryInProgress;
            return processed;
        }

        const std::uint64_t sequence = next_ingress_sequence++;
        const auto previous_state = core->save_state();
        const auto previous_candidates = candidates;
        if (const auto* published =
                std::get_if<CandidateTablePublishedEvent>(&payload);
            published != nullptr && published->table) {
            candidates = published->table;
        }
        ApplyResult applied = core->apply(CoreEvent{sequence, std::move(payload)});
        processed.receipt.ingress_sequence = sequence;
        processed.receipt.error = applied.command_error;
        processed.receipt.idempotent_replay = applied.idempotent_replay;
        if (!applied.command_accepted) {
            rollback_core(previous_state, previous_candidates, sequence);
            processed.receipt.ingress_sequence = 0U;
            return processed;
        }

        std::vector<HeldNormalReservation> reservations;
        reservations.reserve(applied.effects.size());
        for (std::size_t index = 0U; index < applied.effects.size(); ++index) {
            const CoreEffect& effect = applied.effects[index];
            if (is_cancel_effect(effect)) {
                continue;
            }
            const VenueId venue = effect_venue(effect);
            const auto queue = egress.find(venue);
            if (queue == egress.end()) {
                rollback_core(previous_state, previous_candidates, sequence);
                fail_stopped = true;
                processed.receipt.ingress_sequence = 0U;
                processed.receipt.error = CommandError::JournalFailure;
                return processed;
            }
            auto reservation = queue->second->try_reserve_normal();
            if (!reservation.has_value()) {
                rollback_core(previous_state, previous_candidates, sequence);
                processed.receipt.ingress_sequence = 0U;
                processed.receipt.error = CommandError::IngressSaturated;
                return processed;
            }
            reservations.push_back(HeldNormalReservation{
                queue->second.get(), std::move(*reservation), index});
        }

        std::map<VenueId, std::size_t> required_cancel_capacity;
        for (const auto& effect : applied.effects) {
            if (!is_cancel_effect(effect)) {
                continue;
            }
            const VenueId venue = effect_venue(effect);
            const auto queue = egress.find(venue);
            if (queue == egress.end()) {
                rollback_core(previous_state, previous_candidates, sequence);
                fail_stopped = true;
                processed.receipt.ingress_sequence = 0U;
                processed.receipt.error = CommandError::JournalFailure;
                return processed;
            }
            ++required_cancel_capacity[venue];
        }
        for (const auto& [venue, required] : required_cancel_capacity) {
            if (!egress.at(venue)->cancel_has_capacity(required)) {
                rollback_core(previous_state, previous_candidates, sequence);
                processed.receipt.ingress_sequence = 0U;
                processed.receipt.error = CommandError::IngressSaturated;
                return processed;
            }
        }

        persistence::BatchCommit commit;
        std::vector<std::byte> state_image;
        try {
            auto inputs = journal_inputs(applied.records);
            state_image = encode_runtime_state(rib, *candidates, *core);
            inputs.push_back(persistence::RecordInput{
                runtime_state_record_type,
                runtime_state_version,
                sequence,
                state_image,
            });
            commit = journal.append_batch(inputs);
        } catch (const std::exception&) {
            rollback_core(previous_state, previous_candidates, sequence);
            fail_stopped = true;
            processed.receipt.ingress_sequence = 0U;
            processed.receipt.error = CommandError::JournalFailure;
            return processed;
        }
        processed.receipt.journal_record_id = commit.last_record_id;
        ++batches_since_checkpoint;
        if (batches_since_checkpoint >= checkpoint_interval_batches) {
            try {
                persistence::CheckpointStore::write_atomic(
                    checkpoint_path,
                    journal.header(),
                    persistence::Checkpoint{
                        commit.last_record_id,
                        commit.batch_id,
                        std::move(state_image),
                    });
                batches_since_checkpoint = 0U;
            } catch (const std::exception&) {
                // The journal batch is already durable. A checkpoint is only
                // a replay optimization and is retried after the next batch.
            }
        }

        for (auto& held : reservations) {
            if (!held.queue->commit(
                    std::move(held.reservation),
                    std::move(applied.effects[held.effect_index]))) {
                fail_stopped = true;
                processed.receipt.error = CommandError::JournalFailure;
                return processed;
            }
        }
        for (std::size_t index = 0U; index < applied.effects.size(); ++index) {
            CoreEffect& effect = applied.effects[index];
            if (!is_cancel_effect(effect)) {
                continue;
            }
            const VenueId venue = effect_venue(effect);
            const auto queue = egress.find(venue);
            if (queue == egress.end() ||
                !queue->second->try_push_cancel(std::move(effect))) {
                fail_stopped = true;
                processed.receipt.error = CommandError::JournalFailure;
                return processed;
            }
        }

        processed.receipt.status = CommandStatus::Accepted;
        processed.receipt.error = CommandError::None;
        processed.reports = std::move(applied.reports);
        return processed;
    }

    DeliveryTicket enqueue_reports(std::vector<ParentReport> reports) {
        require_coordinator();
        if (reports.empty()) {
            return {};
        }
        auto completed = std::make_shared<std::promise<void>>();
        DeliveryTicket ticket{completed->get_future().share()};
        if (!report_queue->wait_push(ReportWork{
                ReportBatch{std::move(reports), std::move(completed)}})) {
            fail_stopped = true;
            throw std::runtime_error("runtime report worker is stopped");
        }
        return ticket;
    }

    Reported<CommandReceipt> process_reported(
        CoreEventPayload payload,
        const bool public_command) {
        Processed processed =
            process_on_coordinator(std::move(payload), public_command);
        DeliveryTicket delivery = enqueue_reports(std::move(processed.reports));
        return Reported<CommandReceipt>{
            std::move(processed.receipt), std::move(delivery)};
    }

    Admission admission_for(const AdapterEventPayload& event) {
        return std::visit(
            [this](const auto& payload) -> Admission {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, RuntimeViewPublishedEvent>) {
                    const auto found = venue_ingress.find(payload.view.venue);
                    if (found == venue_ingress.end()) {
                        return Admission{nullptr, false, false};
                    }
                    const bool not_live =
                        payload.view.market_data_state != MarketDataState::Live ||
                        !payload.view.feed_sufficiently_fresh ||
                        !payload.view.book.has_value() ||
                        payload.view.book->feed_state != MarketDataState::Live;
                    return Admission{
                        found->second.market_data.get(), not_live, true};
                } else if constexpr (
                    std::is_same_v<T, OrderEntrySessionEvent>) {
                    const auto found = venue_ingress.find(payload.venue);
                    if (found == venue_ingress.end()) {
                        return Admission{nullptr, false, false};
                    }
                    return Admission{
                        found->second.order_entry.get(),
                        payload.state != OrderEntryState::Ready,
                        true};
                } else if constexpr (
                    std::is_same_v<T, LocalDispatchEvent>) {
                    const auto found = venue_ingress.find(payload.result.venue);
                    return found == venue_ingress.end()
                               ? Admission{nullptr, false, false}
                               : Admission{
                                     found->second.order_entry.get(), false, true};
                } else if constexpr (
                    std::is_same_v<T, ExecutionReportEvent>) {
                    const auto found = venue_ingress.find(payload.report.venue);
                    return found == venue_ingress.end()
                               ? Admission{nullptr, false, false}
                               : Admission{
                                     found->second.order_entry.get(), false, true};
                } else {
                    // Timeouts and reconciliation are risk/recovery control
                    // events. They use reserved global capacity.
                    return Admission{nullptr, true, true};
                }
            },
            event);
    }

    std::optional<Reported<bool>> publish_admitted(AdapterEventPayload event) {
        const Admission admission = admission_for(event);
        if (!admission.valid) {
            return Reported<bool>{false, {}};
        }
        if (admission.gate != nullptr) {
            const bool acquired = admission.critical
                                      ? admission.gate->wait_acquire()
                                      : admission.gate->try_acquire();
            if (!acquired) {
                return std::nullopt;
            }
        }

        auto promise =
            std::make_shared<std::promise<Reported<bool>>>();
        auto future = promise->get_future();
        CoordinatorWork work =
            [this,
             event = std::move(event),
             gate = admission.gate,
             promise]() mutable {
                try {
                    std::optional<ChildOrderId> reconciled_child;
                    if (const auto* reconciliation =
                            std::get_if<ReconciliationEvent>(&event);
                        reconciliation != nullptr) {
                        reconciled_child = reconciliation->child;
                    }
                    CoreEventPayload core_event = std::visit(
                        [](auto&& payload) -> CoreEventPayload {
                            return CoreEventPayload{
                                std::forward<decltype(payload)>(payload)};
                        },
                        std::move(event));
                    Processed processed = process_on_coordinator(
                        std::move(core_event), false);
                    if (processed.receipt.accepted() &&
                        reconciled_child.has_value()) {
                        recovery_unreconciled.erase(*reconciled_child);
                    }
                    DeliveryTicket delivery =
                        enqueue_reports(std::move(processed.reports));
                    if (gate != nullptr) {
                        gate->release();
                    }
                    promise->set_value(Reported<bool>{
                        processed.receipt.accepted(),
                        std::move(delivery)});
                } catch (...) {
                    if (gate != nullptr) {
                        gate->release();
                    }
                    promise->set_exception(std::current_exception());
                }
            };

        const bool queued = admission.critical
                                ? coordinator_queue->wait_push_critical(
                                      std::move(work))
                                : coordinator_queue->try_push_normal(
                                      std::move(work));
        if (!queued) {
            if (admission.gate != nullptr) {
                admission.gate->release();
            }
            return std::nullopt;
        }
        return future.get();
    }

    Reported<CommandReceipt> set_venue_admin_on_coordinator(
        const VenueId venue,
        const AdminState state) {
        require_coordinator();
        Processed processed;
        const std::vector<RouteConfig> previous_routes(
            rib.routes().begin(), rib.routes().end());
        const std::uint64_t previous_revision = rib.revision();
        if (!rib.set_admin(venue, state)) {
            processed.receipt.error = CommandError::InvalidCommand;
            return Reported<CommandReceipt>{processed.receipt, {}};
        }
        const auto published = compiler.compile(rib);
        processed = process_on_coordinator(
            CandidateTablePublishedEvent{published}, false);
        if (!processed.receipt.accepted()) {
            std::vector<RouteValidationError> errors;
            if (!rib.replace_all(previous_routes, errors) ||
                !rib.restore_revision(previous_revision)) {
                fail_stopped = true;
                processed.receipt.error = CommandError::JournalFailure;
            }
        }
        DeliveryTicket delivery = enqueue_reports(std::move(processed.reports));
        return Reported<CommandReceipt>{
            std::move(processed.receipt), std::move(delivery)};
    }

    Reported<std::optional<CoreEffect>> poll_egress_on_coordinator(
        const VenueId venue) {
        require_coordinator();
        const auto found = egress.find(venue);
        if (found == egress.end() || fail_stopped) {
            return Reported<std::optional<CoreEffect>>{std::nullopt, {}};
        }
        auto pending = found->second->try_pop();
        if (!pending.has_value()) {
            return Reported<std::optional<CoreEffect>>{std::nullopt, {}};
        }

        CoreEffect effect = std::move(pending->second);
        const SorSnapshot snapshot = core->snapshot();
        if (snapshot.last_dispatch_attempt_id ==
            std::numeric_limits<std::uint64_t>::max()) {
            fail_stopped = true;
            return Reported<std::optional<CoreEffect>>{std::nullopt, {}};
        }
        const std::uint64_t attempt_id =
            snapshot.last_dispatch_attempt_id + 1U;
        Processed started = process_on_coordinator(
            dispatch_started_event(effect, attempt_id), false);
        if (!started.receipt.accepted()) {
            fail_stopped = true;
            return Reported<std::optional<CoreEffect>>{std::nullopt, {}};
        }
        assign_dispatch_attempt(effect, attempt_id);
        DeliveryTicket delivery = enqueue_reports(std::move(started.reports));
        return Reported<std::optional<CoreEffect>>{
            std::move(effect), std::move(delivery)};
    }

    Reported<bool> complete_recovery_on_coordinator() {
        require_coordinator();
        if (fail_stopped) {
            return Reported<bool>{false, {}};
        }
        if (!recovery_is_pending) {
            return Reported<bool>{true, {}};
        }
        if (!recovery_unreconciled.empty() ||
            !core->recovery_consistent()) {
            return Reported<bool>{false, {}};
        }
        Processed processed =
            process_on_coordinator(RoutingGateEvent{true}, false);
        if (processed.receipt.accepted()) {
            recovery_is_pending = false;
        }
        DeliveryTicket delivery = enqueue_reports(std::move(processed.reports));
        return Reported<bool>{
            processed.receipt.accepted(), std::move(delivery)};
    }

    void change_report_sink(IParentReportSink* sink) {
        auto completed = std::make_shared<std::promise<void>>();
        auto future = completed->get_future();
        if (!report_queue->wait_push(
                ReportWork{SinkChange{sink, std::move(completed)}})) {
            throw std::runtime_error("runtime report worker is stopped");
        }
        future.get();
    }
};

SorRuntime::SorRuntime(
    RuntimeConfiguration configuration,
    std::shared_ptr<const adapters::WireIdentityFactory>
        input_wire_identity_factory)
    : impl_(std::make_unique<Impl>(
          std::move(configuration),
          std::move(input_wire_identity_factory))) {}

SorRuntime::~SorRuntime() = default;
SorRuntime::SorRuntime(SorRuntime&&) noexcept = default;
SorRuntime& SorRuntime::operator=(SorRuntime&&) noexcept = default;

CommandReceipt SorRuntime::submit(const ParentOrder& order) {
    auto result = impl_->invoke_try([this, order] {
        return impl_->process_reported(ParentSubmitEvent{order}, true);
    });
    if (!result.has_value()) {
        CommandReceipt saturated;
        saturated.error = CommandError::IngressSaturated;
        return saturated;
    }

    return std::move(result->value);
}

CommandReceipt SorRuntime::cancel(const ParentOrderId parent) {
    auto result = impl_->invoke_wait(
        [this, parent] {
            return impl_->process_reported(ParentCancelEvent{parent}, true);
        },
        true);

    return std::move(result.value);
}

CommandReceipt SorRuntime::replace(const ParentReplace& replacement) {
    auto result = impl_->invoke_try([this, replacement] {
        return impl_->process_reported(
            ParentReplaceEvent{replacement}, true);
    });
    if (!result.has_value()) {
        CommandReceipt saturated;
        saturated.error = CommandError::IngressSaturated;
        return saturated;
    }

    return std::move(result->value);
}

RouteTrace SorRuntime::trace(const ParentOrder& order) const {
    return impl_->invoke_wait([this, order] {
        impl_->require_coordinator();
        return impl_->core->trace(order);
    });
}

SorSnapshot SorRuntime::inspect() const {
    return impl_->invoke_wait([this] {
        impl_->require_coordinator();
        return impl_->core->snapshot();
    });
}

void SorRuntime::set_report_sink(IParentReportSink& sink) {
    impl_->change_report_sink(&sink);
}

void SorRuntime::clear_report_sink() {
    impl_->change_report_sink(nullptr);
}

CommandReceipt SorRuntime::set_venue_admin(
    const VenueId venue,
    const AdminState state) {
    const bool critical = state == AdminState::Down;
    if (critical) {
        auto result = impl_->invoke_wait(
            [this, venue, state] {
                return impl_->set_venue_admin_on_coordinator(venue, state);
            },
            true);

        return std::move(result.value);
    }
    auto result = impl_->invoke_try([this, venue, state] {
        return impl_->set_venue_admin_on_coordinator(venue, state);
    });
    if (!result.has_value()) {
        CommandReceipt saturated;
        saturated.error = CommandError::IngressSaturated;
        return saturated;
    }

    return std::move(result->value);
}

CommandReceipt SorRuntime::set_kill_switch(
    const bool active,
    std::string reason) {
    if (active) {
        auto result = impl_->invoke_wait(
            [this, active, reason = std::move(reason)]() mutable {
                return impl_->process_reported(
                    KillSwitchEvent{active, std::move(reason)}, false);
            },
            true);

        return std::move(result.value);
    }
    auto result = impl_->invoke_try(
        [this, active, reason = std::move(reason)]() mutable {
            return impl_->process_reported(
                KillSwitchEvent{active, std::move(reason)}, false);
        });
    if (!result.has_value()) {
        CommandReceipt saturated;
        saturated.error = CommandError::IngressSaturated;
        return saturated;
    }

    return std::move(result->value);
}

bool SorRuntime::publish(AdapterEventPayload event) {
    auto result = impl_->publish_admitted(std::move(event));
    if (!result.has_value()) {
        return false;
    }

    return result->value;
}

std::optional<CoreEffect> SorRuntime::poll_egress(const VenueId venue) {
    auto result = impl_->invoke_wait([this, venue] {
        return impl_->poll_egress_on_coordinator(venue);
    });

    return std::move(result.value);
}

bool SorRuntime::complete_recovery() {
    auto result = impl_->invoke_wait([this] {
        return impl_->complete_recovery_on_coordinator();
    });

    return result.value;
}

bool SorRuntime::recovery_pending() const {
    return impl_->invoke_wait([this] {
        impl_->require_coordinator();
        return impl_->recovery_is_pending;
    });
}

bool SorRuntime::failed() const {
    return impl_->invoke_wait([this] {
        impl_->require_coordinator();
        return impl_->fail_stopped;
    });
}

std::uint64_t SorRuntime::report_callback_failures() const {
    return impl_->report_callback_failure_count.load(std::memory_order_relaxed);
}

}  // namespace sorx

