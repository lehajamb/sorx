#include "sorx/adapters/mockx_venue_runtime.hpp"

#include "sorx/runtime_queues.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace sorx::adapters {
namespace {

using OrderEntryCallback = std::variant<
    OrderEntrySessionEvent,
    LocalDispatchResult,
    NormalizedExecutionReport>;

[[nodiscard]] bool valid_market_data_state(
    const MarketDataState state) noexcept {
    return state == MarketDataState::Snapshotting ||
           state == MarketDataState::Stitching ||
           state == MarketDataState::Live ||
           state == MarketDataState::Recovering ||
           state == MarketDataState::Failed;
}

[[nodiscard]] bool terminal_child(const ChildState state) noexcept {
    return state == ChildState::Filled ||
           state == ChildState::Canceled ||
           state == ChildState::Rejected;
}

[[nodiscard]] MarketDataState market_data_state(
    const MockXMdSessionState state) noexcept {
    switch (state) {
        case MockXMdSessionState::Starting:
        case MockXMdSessionState::WaitingForLiveIdentity:
        case MockXMdSessionState::Snapshotting:
            return MarketDataState::Snapshotting;
        case MockXMdSessionState::Stitching:
            return MarketDataState::Stitching;
        case MockXMdSessionState::Live:
            return MarketDataState::Live;
        case MockXMdSessionState::Recovering:
            return MarketDataState::Recovering;
        case MockXMdSessionState::Stopped:
        case MockXMdSessionState::Stopping:
        case MockXMdSessionState::Failed:
            return MarketDataState::Failed;
    }
    return MarketDataState::Failed;
}

struct CallbackState final :
    MockXMdPublicationSink,
    quickfix::IQuickFixSessionSink {
    CallbackState(
        const std::size_t market_data_capacity,
        const std::size_t order_entry_capacity,
        const std::size_t dispatch_capacity)
        : market_data(market_data_capacity),
          order_entry(order_entry_capacity),
          dispatch(dispatch_capacity) {}

    [[nodiscard]] bool try_publish(
        MockXMdPublication publication) noexcept override {
        if (!accepting.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            if (market_data.try_push(std::move(publication))) {
                market_data_callbacks.fetch_add(1U, std::memory_order_relaxed);
                wake.notify_one();
                return true;
            }
        } catch (...) {
        }
        market_data_losses.fetch_add(1U, std::memory_order_relaxed);
        market_data_loss_pending.store(true, std::memory_order_release);
        wake.notify_one();
        return false;
    }

    [[nodiscard]] bool try_publish(
        OrderEntrySessionEvent event) override {
        return try_publish_order_entry(OrderEntryCallback{std::move(event)});
    }

    [[nodiscard]] bool try_publish(LocalDispatchResult result) override {
        if (!accepting.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            if (dispatch.try_push(
                    OrderEntryCallback{std::move(result)})) {
                order_entry_callbacks.fetch_add(1U, std::memory_order_relaxed);
                wake.notify_one();
                return true;
            }
        } catch (...) {
        }
        order_entry_losses.fetch_add(1U, std::memory_order_relaxed);
        dispatch_loss_pending.store(true, std::memory_order_release);
        wake.notify_one();
        return false;
    }

    [[nodiscard]] bool try_publish(
        NormalizedExecutionReport report) override {
        return try_publish_order_entry(OrderEntryCallback{std::move(report)});
    }

    [[nodiscard]] bool empty() const {
        return market_data.size() == 0U &&
               order_entry.size() == 0U && dispatch.size() == 0U;
    }

    [[nodiscard]] bool has_pending_control() const noexcept {
        return market_data_loss_pending.load(std::memory_order_acquire) ||
               order_entry_loss_pending.load(std::memory_order_acquire) ||
               dispatch_loss_pending.load(std::memory_order_acquire);
    }

    void notify() noexcept {
        wake.notify_all();
    }

    runtime::BoundedQueue<MockXMdPublication> market_data;
    runtime::BoundedQueue<OrderEntryCallback> order_entry;
    runtime::BoundedQueue<OrderEntryCallback> dispatch;
    std::atomic<bool> accepting{true};
    std::atomic<bool> market_data_loss_pending{};
    std::atomic<bool> order_entry_loss_pending{};
    std::atomic<bool> dispatch_loss_pending{};
    std::atomic<std::uint64_t> market_data_callbacks{};
    std::atomic<std::uint64_t> order_entry_callbacks{};
    std::atomic<std::uint64_t> market_data_losses{};
    std::atomic<std::uint64_t> order_entry_losses{};
    mutable std::mutex wake_mutex;
    std::condition_variable wake;

private:
    [[nodiscard]] bool try_publish_order_entry(
        OrderEntryCallback callback) noexcept {
        if (!accepting.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            if (order_entry.try_push(std::move(callback))) {
                order_entry_callbacks.fetch_add(1U, std::memory_order_relaxed);
                wake.notify_one();
                return true;
            }
        } catch (...) {
        }
        order_entry_losses.fetch_add(1U, std::memory_order_relaxed);
        order_entry_loss_pending.store(true, std::memory_order_release);
        wake.notify_one();
        return false;
    }
};

void validate_config(
    const MockXVenueRuntimeConfig& config,
    const MockXVenueTestSeams& seams) {
    const VenueId venue = config.order_entry.venue;
    if (!venue || config.order_entry.session_epoch == 0U ||
        config.market_data.adapter.feed.venue != venue ||
        config.market_data_callback_capacity == 0U ||
        config.order_entry_callback_capacity == 0U ||
        config.local_dispatch_callback_capacity == 0U ||
        config.worker_drain_batch == 0U ||
        config.worker_idle_wait.count() <= 0 ||
        config.market_data.adapter.instruments.empty()) {
        throw std::invalid_argument(
            "invalid MockX venue-runtime configuration");
    }
    std::vector<InstrumentId> instruments;
    instruments.reserve(config.market_data.adapter.instruments.size());
    for (const auto& binding : config.market_data.adapter.instruments) {
        if (binding.stock_locate == 0U || binding.symbol.empty() ||
            !binding.instrument ||
            std::find(instruments.begin(), instruments.end(),
                      binding.instrument) != instruments.end()) {
            throw std::invalid_argument(
                "invalid or duplicate MockX venue instrument binding");
        }
        instruments.push_back(binding.instrument);
    }
    if (seams.disable_native_market_data &&
        !seams.order_entry_transport.has_value()) {
        throw std::invalid_argument(
            "offline market data requires an offline order-entry transport");
    }
}

}  // namespace

struct MockXVenueRuntime::Impl final {
    Impl(
        SorRuntime& input_runtime,
        MockXVenueRuntimeConfig input_config,
        MockXVenueTestSeams input_seams)
        : runtime(input_runtime),
          config(std::move(input_config)),
          seams(std::move(input_seams)) {
        validate_config(config, seams);
        if (!runtime.recovery_pending()) {
            throw std::invalid_argument(
                "MockX venue runtime must be constructed after replay and before recovery completion");
        }

        callbacks = std::make_shared<CallbackState>(
            config.market_data_callback_capacity,
            config.order_entry_callback_capacity,
            config.local_dispatch_callback_capacity);
        for (const auto& binding : config.market_data.adapter.instruments) {
            books.emplace(binding.instrument, std::nullopt);
        }

        auto initial = std::make_shared<MockXVenueAdjacencyView>();
        initial->venue = venue();
        initial->candidates.reserve(books.size());
        for (const auto& [instrument, ignored] : books) {
            static_cast<void>(ignored);
            CandidateRuntimeView candidate;
            candidate.venue = venue();
            candidate.market_data_state = MarketDataState::Snapshotting;
            candidate.order_entry_state = OrderEntryState::Disconnected;
            candidate.instrument_state = InstrumentState::Unknown;
            candidate.feed_sufficiently_fresh = false;
            initial->candidates.push_back(std::move(candidate));
            instrument_order.push_back(instrument);
        }
        adjacency.store(std::move(initial), std::memory_order_release);

        if (seams.order_entry_transport.has_value()) {
            order_entry = std::make_unique<quickfix::QuickFixSessionAdapter>(
                config.order_entry,
                *callbacks,
                std::move(*seams.order_entry_transport));
        } else {
            order_entry = std::make_unique<quickfix::QuickFixSessionAdapter>(
                config.order_entry,
                *callbacks);
        }
        if (!seams.disable_native_market_data) {
            market_data = std::make_unique<MockXMdSession>(
                config.market_data,
                callbacks);
        }
    }

    ~Impl() {
        stop();
    }

    [[nodiscard]] VenueId venue() const noexcept {
        return config.order_entry.venue;
    }

    void set_error_detail(std::string detail) noexcept {
        try {
            std::lock_guard lock(error_mutex);
            error_detail = std::move(detail);
        } catch (...) {
        }
    }

    void record_failure(
        const MockXVenueRuntimeError error,
        std::string detail) noexcept {
        last_error_code.store(error, std::memory_order_release);
        set_error_detail(std::move(detail));
        if (error !=
                MockXVenueRuntimeError::MarketDataCallbackQueueFull &&
            error != MockXVenueRuntimeError::MarketDataSessionFailure) {
            order_entry_usable.store(false, std::memory_order_release);
        }
        fatal_failure.store(true, std::memory_order_release);
        failure_invalidation_pending.store(true, std::memory_order_release);
        lifecycle.store(MockXVenueRuntimeState::Failed,
                        std::memory_order_release);
        callbacks->notify();
    }

    [[nodiscard]] bool publish_runtime(AdapterEventPayload event) noexcept {
        try {
            if (runtime.publish(std::move(event))) {
                runtime_events_published.fetch_add(1U,
                                                   std::memory_order_relaxed);
                return true;
            }
        } catch (...) {
        }
        runtime_publication_failures.fetch_add(1U,
                                               std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] bool feed_ready() const noexcept {
        if (current_market_data != MarketDataState::Live || books.empty()) {
            return false;
        }
        return std::all_of(
            books.begin(), books.end(), [](const auto& item) {
                return item.second.has_value() && item.second->valid() &&
                       item.second->feed_state == MarketDataState::Live;
            });
    }

    [[nodiscard]] std::shared_ptr<const MockXVenueAdjacencyView>
    make_adjacency(const bool failed_closed) {
        auto output = std::make_shared<MockXVenueAdjacencyView>();
        output->venue = venue();
        output->generation = ++adjacency_generation;
        output->market_data_state = current_market_data;
        output->order_entry_state = current_order_entry;
        output->egress_state = current_egress;
        output->feed_ready = feed_ready();
        output->order_entry_ready =
            current_order_entry == OrderEntryState::Ready;
        output->failed_closed = failed_closed;
        output->candidates.reserve(instrument_order.size());

        for (const auto instrument : instrument_order) {
            CandidateRuntimeView candidate;
            candidate.venue = venue();
            candidate.adjacency_generation = output->generation;
            const auto found = books.find(instrument);
            if (current_market_data == MarketDataState::Live &&
                found != books.end() && found->second.has_value()) {
                candidate.book = found->second;
            }
            candidate.market_data_state = current_market_data;
            candidate.order_entry_state = current_order_entry;
            candidate.egress_state = current_egress;
            candidate.instrument_state = candidate.book.has_value()
                                             ? candidate.book->trading_state
                                             : InstrumentState::Unknown;
            candidate.feed_sufficiently_fresh =
                output->feed_ready && candidate.book.has_value();
            candidate.conversion_compatible = true;
            candidate.risk_allowed = !failed_closed;
            output->candidates.push_back(std::move(candidate));
        }
        return output;
    }

    [[nodiscard]] bool publish_adjacency(const bool failed_closed) noexcept {
        std::shared_ptr<const MockXVenueAdjacencyView> next;
        try {
            next = make_adjacency(failed_closed);
        } catch (...) {
            return false;
        }
        adjacency.store(next, std::memory_order_release);
        for (std::size_t index = 0U;
             index < next->candidates.size(); ++index) {
            if (!publish_runtime(RuntimeViewPublishedEvent{
                    instrument_order[index], next->candidates[index]})) {
                return false;
            }
        }
        return true;
    }

    void apply_failure_invalidation() noexcept {
        current_egress = EgressState::Congested;
        const auto error = last_error_code.load(std::memory_order_acquire);
        if (error == MockXVenueRuntimeError::MarketDataCallbackQueueFull ||
            error == MockXVenueRuntimeError::MarketDataSessionFailure) {
            current_market_data = MarketDataState::Failed;
            for (auto& [instrument, book] : books) {
                static_cast<void>(instrument);
                book.reset();
            }
        } else {
            current_market_data = MarketDataState::Failed;
            current_order_entry = OrderEntryState::Degraded;
            for (auto& [instrument, book] : books) {
                static_cast<void>(instrument);
                book.reset();
            }
            static_cast<void>(publish_runtime(OrderEntrySessionEvent{
                venue(),
                config.order_entry.session_epoch,
                OrderEntryState::Degraded,
            }));
        }
        static_cast<void>(publish_adjacency(true));
    }

    void handle_callback_losses() noexcept {
        if (callbacks->market_data_loss_pending.exchange(
                false, std::memory_order_acq_rel)) {
            record_failure(
                MockXVenueRuntimeError::MarketDataCallbackQueueFull,
                "bounded market-data callback queue exhausted");
        }
        if (callbacks->order_entry_loss_pending.exchange(
                false, std::memory_order_acq_rel)) {
            record_failure(
                MockXVenueRuntimeError::OrderEntryCallbackQueueFull,
                "bounded order-entry callback queue exhausted");
        }
        if (callbacks->dispatch_loss_pending.exchange(
                false, std::memory_order_acq_rel)) {
            record_failure(
                MockXVenueRuntimeError::LocalDispatchPublicationLost,
                "bounded local-dispatch callback queue exhausted");
        }
        if (failure_invalidation_pending.exchange(
                false, std::memory_order_acq_rel)) {
            apply_failure_invalidation();
        }
    }

    [[nodiscard]] bool valid_publication_book(
        const md::BookView& book) const noexcept {
        return book.venue == venue() && book.valid() &&
               books.contains(book.instrument);
    }

    void handle_market_data(MockXMdPublication publication) noexcept {
        std::visit(
            [this](auto&& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, md::EngineUpdate>) {
                    if (!valid_market_data_state(payload.state) ||
                        std::any_of(
                            payload.publications.begin(),
                            payload.publications.end(),
                            [this](const md::BookView& book) {
                                return !valid_publication_book(book);
                            })) {
                        record_failure(
                            MockXVenueRuntimeError::MarketDataSessionFailure,
                            "invalid protocol-neutral market-data publication");
                        return;
                    }
                    current_market_data = payload.state;
                    if (payload.state != MarketDataState::Live) {
                        for (auto& [instrument, book] : books) {
                            static_cast<void>(instrument);
                            book.reset();
                        }
                    }
                    for (const auto& book : payload.publications) {
                        books[book.instrument] = book;
                    }
                    if (!publish_adjacency(
                            fatal_failure.load(std::memory_order_acquire))) {
                        record_failure(
                            MockXVenueRuntimeError::RuntimePublicationRejected,
                            "SorRuntime rejected a market-data adjacency event");
                    }
                } else {
                    current_market_data = market_data_state(payload.current);
                    if (current_market_data != MarketDataState::Live) {
                        for (auto& [instrument, book] : books) {
                            static_cast<void>(instrument);
                            book.reset();
                        }
                    }
                    if (payload.current == MockXMdSessionState::Failed ||
                        payload.error != MockXMdSessionError::None) {
                        record_failure(
                            MockXVenueRuntimeError::MarketDataSessionFailure,
                            "native MockX market-data session failed");
                        return;
                    }
                    if (!publish_adjacency(
                            fatal_failure.load(std::memory_order_acquire))) {
                        record_failure(
                            MockXVenueRuntimeError::RuntimePublicationRejected,
                            "SorRuntime rejected market-data health withdrawal");
                    }
                }
            },
            std::move(publication.payload));
    }

    void degrade_order_entry(std::string detail) noexcept {
        current_egress = EgressState::Congested;
        lifecycle.store(MockXVenueRuntimeState::Degraded,
                        std::memory_order_release);
        last_error_code.store(
            MockXVenueRuntimeError::OrderEntrySessionFailure,
            std::memory_order_release);
        set_error_detail(std::move(detail));
    }

    void handle_order_entry(OrderEntryCallback callback) noexcept {
        std::visit(
            [this](auto&& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, OrderEntrySessionEvent>) {
                    if (payload.venue != venue() ||
                        payload.session_epoch !=
                            config.order_entry.session_epoch) {
                        record_failure(
                            MockXVenueRuntimeError::OrderEntrySessionFailure,
                            "QuickFIX callback has the wrong venue/session identity");
                        return;
                    }
                    current_order_entry = payload.state;
                    current_egress =
                        payload.state == OrderEntryState::Ready
                            ? EgressState::Ready
                            : EgressState::Congested;
                    if (!publish_runtime(payload)) {
                        record_failure(
                            MockXVenueRuntimeError::RuntimePublicationRejected,
                            "SorRuntime rejected an order-entry session event");
                        return;
                    }
                    if (payload.state == OrderEntryState::Degraded) {
                        record_failure(
                            MockXVenueRuntimeError::OrderEntrySessionFailure,
                            "QuickFIX session degraded");
                        return;
                    }
                    if (payload.state == OrderEntryState::Disconnected &&
                        !stop_started.load(std::memory_order_acquire)) {
                        degrade_order_entry("QuickFIX session disconnected");
                    }
                    if (!publish_adjacency(
                            fatal_failure.load(std::memory_order_acquire))) {
                        record_failure(
                            MockXVenueRuntimeError::RuntimePublicationRejected,
                            "SorRuntime rejected an order-entry adjacency event");
                    }
                } else if constexpr (
                    std::is_same_v<T, LocalDispatchResult>) {
                    const LocalDispatchState dispatch_state = payload.state;
                    const std::string dispatch_detail = payload.detail;
                    if (!publish_runtime(LocalDispatchEvent{
                            std::move(payload)})) {
                        record_failure(
                            MockXVenueRuntimeError::RuntimePublicationRejected,
                            "SorRuntime rejected a local dispatch result");
                    } else if (
                        dispatch_state !=
                            LocalDispatchState::AcceptedLocally) {
                        record_failure(
                            MockXVenueRuntimeError::OrderEntrySessionFailure,
                            "non-definitive local dispatch outcome: " +
                                dispatch_detail);
                    }
                } else {
                    if (!publish_runtime(ExecutionReportEvent{
                            std::move(payload)})) {
                        record_failure(
                            MockXVenueRuntimeError::RuntimePublicationRejected,
                            "SorRuntime rejected a normalized execution report");
                    }
                }
            },
            std::move(callback));
    }

    [[nodiscard]] bool test_egress_permitted() noexcept {
        if (!seams.permit_egress) {
            return true;
        }
        try {
            return seams.permit_egress();
        } catch (...) {
            record_failure(
                MockXVenueRuntimeError::WorkerFailure,
                "offline egress permit callback threw");
            return false;
        }
    }

    void publish_lost_dispatch_directly(
        LocalDispatchResult result) noexcept {
        if (!publish_runtime(LocalDispatchEvent{std::move(result)})) {
            record_failure(
                MockXVenueRuntimeError::RuntimePublicationRejected,
                "SorRuntime rejected the recovered local dispatch result");
        }
    }

    void dispatch_effect(CoreEffect effect) noexcept {
        const bool callback_loss_before =
            callbacks->dispatch_loss_pending.load(std::memory_order_acquire);
        LocalDispatchResult result = std::visit(
            [this](auto&& dispatch) -> LocalDispatchResult {
                using T = std::decay_t<decltype(dispatch)>;
                if constexpr (std::is_same_v<T, DispatchCancelEffect>) {
                    cancel_dispatches.fetch_add(1U,
                                                std::memory_order_relaxed);
                    return order_entry->send_cancel(dispatch.intent);
                } else {
                    new_dispatches.fetch_add(1U,
                                             std::memory_order_relaxed);
                    return order_entry->send_new(dispatch.intent);
                }
            },
            std::move(effect));
        const bool callback_lost =
            !callback_loss_before &&
            callbacks->dispatch_loss_pending.load(std::memory_order_acquire);
        if (callback_lost) {
            // send_new/send_cancel return the exact result even when their
            // nonblocking callback cannot retain it. Publish it synchronously
            // from the sole bridge worker before withdrawing the venue.
            publish_lost_dispatch_directly(std::move(result));
        }
    }

    [[nodiscard]] bool can_poll_egress() noexcept {
        return order_entry->state() == OrderEntryState::Ready &&
               order_entry_usable.load(std::memory_order_acquire) &&
               !egress_blocked.load(std::memory_order_acquire) &&
               test_egress_permitted();
    }

    bool drain_egress() noexcept {
        if (!can_poll_egress()) {
            return false;
        }
        std::optional<CoreEffect> effect;
        try {
            effect = runtime.poll_egress(venue());
        } catch (...) {
            record_failure(
                MockXVenueRuntimeError::WorkerFailure,
                "SorRuntime egress polling threw");
            return false;
        }
        if (!effect.has_value()) {
            return false;
        }
        if (fatal_failure.load(std::memory_order_acquire) &&
            std::holds_alternative<DispatchNewEffect>(*effect)) {
            const auto& intent =
                std::get<DispatchNewEffect>(*effect).intent;
            LocalDispatchResult unsent;
            unsent.child = intent.child;
            unsent.state = LocalDispatchState::DefinitelyUnsent;
            unsent.detail = "venue failed closed before new dispatch";
            unsent.kind = DispatchAttemptKind::New;
            unsent.attempt_id = intent.dispatch_attempt_id;
            unsent.venue = intent.venue;
            unsent.session_epoch = intent.session_epoch;
            unsent.wire_order_id = intent.wire_order_id;
            publish_lost_dispatch_directly(std::move(unsent));
            egress_blocked.store(true, std::memory_order_release);
            return true;
        }
        dispatch_effect(std::move(*effect));
        // A dispatch callback is always consumed before any reentrant FIX
        // execution callback on the next loop iteration.
        return true;
    }

    [[nodiscard]] bool readiness_satisfied() const noexcept {
        const auto current = adjacency.load(std::memory_order_acquire);
        return current && current->feed_ready &&
               current->order_entry_ready && !current->failed_closed;
    }

    [[nodiscard]] bool attempt_complete_recovery_impl() noexcept {
        if (!readiness_satisfied() ||
            fatal_failure.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard lock(recovery_mutex);
        try {
            if (!runtime.recovery_pending()) {
                return true;
            }
            recovery_attempts.fetch_add(1U, std::memory_order_relaxed);
            if (runtime.complete_recovery()) {
                if (runtime.recovery_pending()) {
                    return false;
                }
                recovery_completions.fetch_add(1U,
                                               std::memory_order_relaxed);
                lifecycle.store(MockXVenueRuntimeState::Operational,
                                std::memory_order_release);
                return true;
            }
        } catch (...) {
            record_failure(
                MockXVenueRuntimeError::WorkerFailure,
                "SorRuntime recovery completion threw");
        }
        return false;
    }

    void worker_loop() noexcept {
        try {
            while (true) {
                bool progressed = false;
                handle_callback_losses();

                for (std::size_t index = 0U;
                     index < config.worker_drain_batch; ++index) {
                    auto callback = callbacks->dispatch.try_pop();
                    if (!callback.has_value()) {
                        break;
                    }
                    handle_order_entry(std::move(*callback));
                    progressed = true;
                }
                for (std::size_t index = 0U;
                     index < config.worker_drain_batch; ++index) {
                    auto callback = callbacks->order_entry.try_pop();
                    if (!callback.has_value()) {
                        break;
                    }
                    handle_order_entry(std::move(*callback));
                    progressed = true;
                }
                for (std::size_t index = 0U;
                     index < config.worker_drain_batch; ++index) {
                    auto publication = callbacks->market_data.try_pop();
                    if (!publication.has_value()) {
                        break;
                    }
                    handle_market_data(std::move(*publication));
                    progressed = true;
                }

                if (config.automatically_complete_recovery &&
                    runtime.recovery_pending() && readiness_satisfied()) {
                    static_cast<void>(attempt_complete_recovery_impl());
                }
                if (!stop_requested.load(std::memory_order_acquire)) {
                    progressed = drain_egress() || progressed;
                }

                if (stop_requested.load(std::memory_order_acquire) &&
                    callbacks->empty() && !callbacks->has_pending_control() &&
                    !failure_invalidation_pending.load(
                        std::memory_order_acquire)) {
                    break;
                }
                if (!progressed) {
                    std::unique_lock lock(callbacks->wake_mutex);
                    callbacks->wake.wait_for(
                        lock,
                        config.worker_idle_wait);
                }
            }
        } catch (...) {
            record_failure(
                MockXVenueRuntimeError::WorkerFailure,
                "unhandled venue bridge worker failure");
            apply_failure_invalidation();
        }
        worker_running.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool start() {
        bool expected = false;
        if (!start_called.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        if (!runtime.recovery_pending() || runtime.failed()) {
            record_failure(
                MockXVenueRuntimeError::InvalidLifecycle,
                "SorRuntime must still be healthy and recovery-only at venue start");
            return false;
        }

        lifecycle.store(MockXVenueRuntimeState::Starting,
                        std::memory_order_release);
        worker_running.store(true, std::memory_order_release);
        worker = std::thread([this] { worker_loop(); });

        // Production QuickFIX start resolves exactly one configured SessionID,
        // constructs FileStoreFactory/SocketInitiator, and restores persistent
        // sequence state before the initiator can issue a logon callback.
        if (!order_entry->start()) {
            record_failure(
                MockXVenueRuntimeError::OrderEntrySessionFailure,
                "QuickFIX session failed to start: " +
                    order_entry->last_error());
            stop();
            return false;
        }
        try {
            if (market_data) {
                market_data->start();
            }
        } catch (const std::exception& exception) {
            record_failure(
                MockXVenueRuntimeError::MarketDataSessionFailure,
                std::string{"MockX market-data session failed to start: "} +
                    exception.what());
            stop();
            return false;
        } catch (...) {
            record_failure(
                MockXVenueRuntimeError::MarketDataSessionFailure,
                "MockX market-data session failed to start");
            stop();
            return false;
        }
        lifecycle.store(MockXVenueRuntimeState::RecoveryOnly,
                        std::memory_order_release);
        return true;
    }

    void stop() noexcept {
        if (!start_called.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (!stop_started.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            if (worker.joinable()) {
                worker.join();
            }
            return;
        }

        if (!fatal_failure.load(std::memory_order_acquire)) {
            lifecycle.store(MockXVenueRuntimeState::Stopping,
                            std::memory_order_release);
        }
        // Stop producers first; their final health callbacks are retained and
        // drained before the worker's lifetime barrier completes.
        if (market_data) {
            market_data->stop();
        }
        order_entry->stop();
        stop_requested.store(true, std::memory_order_release);
        callbacks->notify();
        if (worker.joinable()) {
            worker.join();
        }
        callbacks->accepting.store(false, std::memory_order_release);
        callbacks->market_data.close();
        callbacks->order_entry.close();
        callbacks->dispatch.close();
        if (!fatal_failure.load(std::memory_order_acquire)) {
            lifecycle.store(MockXVenueRuntimeState::Stopped,
                            std::memory_order_release);
        }
    }

    SorRuntime& runtime;
    MockXVenueRuntimeConfig config;
    MockXVenueTestSeams seams;
    std::shared_ptr<CallbackState> callbacks;
    std::unique_ptr<quickfix::QuickFixSessionAdapter> order_entry;
    std::unique_ptr<MockXMdSession> market_data;
    std::map<InstrumentId, std::optional<md::BookView>> books;
    std::vector<InstrumentId> instrument_order;
    std::atomic<std::shared_ptr<const MockXVenueAdjacencyView>> adjacency;
    std::uint64_t adjacency_generation{};
    MarketDataState current_market_data{MarketDataState::Snapshotting};
    OrderEntryState current_order_entry{OrderEntryState::Disconnected};
    EgressState current_egress{EgressState::Ready};
    std::thread worker;
    std::atomic<bool> start_called{};
    std::atomic<bool> stop_started{};
    std::atomic<bool> stop_requested{};
    std::atomic<bool> worker_running{};
    std::atomic<bool> fatal_failure{};
    std::atomic<bool> failure_invalidation_pending{};
    std::atomic<bool> order_entry_usable{true};
    std::atomic<bool> egress_blocked{};
    std::mutex recovery_mutex;
    std::atomic<MockXVenueRuntimeState> lifecycle{
        MockXVenueRuntimeState::Stopped};
    std::atomic<MockXVenueRuntimeError> last_error_code{
        MockXVenueRuntimeError::None};
    mutable std::mutex error_mutex;
    std::string error_detail;
    std::atomic<std::uint64_t> runtime_events_published{};
    std::atomic<std::uint64_t> runtime_publication_failures{};
    std::atomic<std::uint64_t> new_dispatches{};
    std::atomic<std::uint64_t> cancel_dispatches{};
    std::atomic<std::uint64_t> recovery_attempts{};
    std::atomic<std::uint64_t> recovery_completions{};
};

MockXVenueRuntime::MockXVenueRuntime(
    SorRuntime& runtime,
    MockXVenueRuntimeConfig config)
    : impl_(std::make_unique<Impl>(
          runtime,
          std::move(config),
          MockXVenueTestSeams{})) {}

MockXVenueRuntime::MockXVenueRuntime(
    SorRuntime& runtime,
    MockXVenueRuntimeConfig config,
    MockXVenueTestSeams test_seams)
    : impl_(std::make_unique<Impl>(
          runtime,
          std::move(config),
          std::move(test_seams))) {}

MockXVenueRuntime::~MockXVenueRuntime() = default;

bool MockXVenueRuntime::start() {
    return impl_->start();
}

void MockXVenueRuntime::stop() noexcept {
    impl_->stop();
}

bool MockXVenueRuntime::running() const noexcept {
    return impl_->worker_running.load(std::memory_order_acquire);
}

bool MockXVenueRuntime::failed() const noexcept {
    return impl_->fatal_failure.load(std::memory_order_acquire);
}

MockXVenueRuntimeState MockXVenueRuntime::state() const noexcept {
    return impl_->lifecycle.load(std::memory_order_acquire);
}

MockXVenueRuntimeError MockXVenueRuntime::last_error() const noexcept {
    return impl_->last_error_code.load(std::memory_order_acquire);
}

std::string MockXVenueRuntime::last_error_detail() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error_detail;
}

MockXVenueRuntimeCounters MockXVenueRuntime::counters() const noexcept {
    return MockXVenueRuntimeCounters{
        impl_->callbacks->market_data_callbacks.load(
            std::memory_order_relaxed),
        impl_->callbacks->order_entry_callbacks.load(
            std::memory_order_relaxed),
        impl_->callbacks->market_data_losses.load(
            std::memory_order_relaxed),
        impl_->callbacks->order_entry_losses.load(
            std::memory_order_relaxed),
        impl_->runtime_events_published.load(std::memory_order_relaxed),
        impl_->runtime_publication_failures.load(std::memory_order_relaxed),
        impl_->new_dispatches.load(std::memory_order_relaxed),
        impl_->cancel_dispatches.load(std::memory_order_relaxed),
        impl_->recovery_attempts.load(std::memory_order_relaxed),
        impl_->recovery_completions.load(std::memory_order_relaxed),
    };
}

std::shared_ptr<const MockXVenueAdjacencyView>
MockXVenueRuntime::adjacency_view() const noexcept {
    return impl_->adjacency.load(std::memory_order_acquire);
}

std::uint16_t MockXVenueRuntime::local_udp_port() const noexcept {
    return impl_->market_data ? impl_->market_data->local_udp_port() : 0U;
}

bool MockXVenueRuntime::try_publish_market_data(
    MockXMdPublication publication) noexcept {
    if (!impl_->seams.disable_native_market_data || !running()) {
        return false;
    }
    return impl_->callbacks->try_publish(std::move(publication));
}

bool MockXVenueRuntime::ingest_fix_session(
    const quickfix::TransportSessionSignal signal,
    const quickfix::QuickFixSessionIdentity& identity) {
    if (!impl_->seams.order_entry_transport.has_value() || !running()) {
        return false;
    }
    return impl_->order_entry->ingest_transport_session(signal, identity);
}

bool MockXVenueRuntime::ingest_fix_application(
    const quickfix::QuickFixSessionIdentity& identity,
    const std::string_view message_type,
    const std::uint64_t inbound_session_sequence,
    const std::span<const mockx::FixField> fields) {
    if (!impl_->seams.order_entry_transport.has_value() || !running()) {
        return false;
    }
    return impl_->order_entry->ingest_transport_application(
        identity,
        message_type,
        inbound_session_sequence,
        fields);
}

ReconciliationEvent MockXVenueRuntime::conservative_reconciliation(
    const ChildSnapshot& child) noexcept {
    return ReconciliationEvent{
        child.id,
        child.state == ChildState::Prepared
            ? ReconciliationDisposition::DefinitelyUnsent
            : ReconciliationDisposition::Unresolved,
        child.filled,
    };
}

bool MockXVenueRuntime::reconcile_recovered_children() {
    if (!impl_->runtime.recovery_pending()) {
        return false;
    }
    const SorSnapshot snapshot = impl_->runtime.inspect();
    bool accepted = true;
    for (const auto& child : snapshot.children) {
        if (child.venue != impl_->venue() || terminal_child(child.state)) {
            continue;
        }
        accepted = impl_->runtime.publish(
                       conservative_reconciliation(child)) &&
                   accepted;
    }
    return accepted;
}

bool MockXVenueRuntime::attempt_complete_recovery() {
    return impl_->attempt_complete_recovery_impl();
}

}  // namespace sorx::adapters






