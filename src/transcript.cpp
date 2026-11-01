#include "sorx/transcript.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace sorx {
namespace {

using Bytes = std::vector<std::byte>;

constexpr std::array<std::byte, 4U> magic{
    std::byte{'S'}, std::byte{'X'}, std::byte{'T'}, std::byte{'R'}};
constexpr std::uint16_t schema_version = 1U;
constexpr std::size_t transcript_header_size = 10U;
constexpr std::size_t minimum_frame_size = 18U;
constexpr std::uint32_t maximum_collection_size = 1'000'000U;

[[nodiscard]] std::uint32_t checked_u32(
    const std::size_t value,
    const char* const description) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string{description} + " is too large");
    }
    return static_cast<std::uint32_t>(value);
}

class Writer final {
public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void u32(const std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(const std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void i64(const std::int64_t value) {
        u64(std::bit_cast<std::uint64_t>(value));
    }

    void boolean(const bool value) {
        u8(value ? 1U : 0U);
    }

    void string(const std::string& value) {
        u32(checked_u32(value.size(), "transcript string"));
        bytes_.insert(
            bytes_.end(),
            reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size()));
    }

    void blob(const std::span<const std::byte> value) {
        u32(checked_u32(value.size(), "transcript byte field"));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    template <typename Enum>
    void enum8(
        const Enum value,
        const Enum maximum,
        const char* const description) {
        static_assert(std::is_enum_v<Enum>);
        using Underlying = std::underlying_type_t<Enum>;
        const auto raw = static_cast<Underlying>(value);
        const auto limit = static_cast<Underlying>(maximum);
        if (raw > limit) {
            throw std::invalid_argument(
                std::string{"invalid "} + description + " in transcript");
        }
        u8(static_cast<std::uint8_t>(raw));
    }

    [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
    [[nodiscard]] Bytes take() && { return std::move(bytes_); }

private:
    Bytes bytes_;
};

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1U);
        return std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }

    [[nodiscard]] std::uint16_t u16() {
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

    [[nodiscard]] std::uint32_t u32() {
        require(4U);
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint32_t>(bytes_[offset_ + index]);
        }
        offset_ += 4U;
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        require(8U);
        std::uint64_t value = 0U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint64_t>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return value;
    }

    [[nodiscard]] std::int64_t i64() {
        return std::bit_cast<std::int64_t>(u64());
    }

    [[nodiscard]] bool boolean() {
        const std::uint8_t value = u8();
        if (value > 1U) {
            throw std::runtime_error("invalid transcript boolean");
        }
        return value != 0U;
    }

    [[nodiscard]] std::string string() {
        const std::uint32_t size = u32();
        require(size);
        const char* const begin =
            reinterpret_cast<const char*>(bytes_.data() + offset_);
        std::string value(begin, begin + size);
        offset_ += size;
        return value;
    }

    [[nodiscard]] Bytes blob() {
        const std::uint32_t size = u32();
        require(size);
        Bytes value(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return value;
    }

    [[nodiscard]] std::uint32_t count(const char* const description) {
        const std::uint32_t value = u32();
        if (value > maximum_collection_size) {
            throw std::runtime_error(
                std::string{"excessive "} + description + " in transcript");
        }
        return value;
    }

    template <typename Enum>
    [[nodiscard]] Enum enum8(
        const Enum maximum,
        const char* const description) {
        static_assert(std::is_enum_v<Enum>);
        const std::uint8_t raw = u8();
        if (raw > static_cast<std::uint8_t>(maximum)) {
            throw std::runtime_error(
                std::string{"invalid "} + description + " in transcript");
        }
        return static_cast<Enum>(raw);
    }

    void require_end() const {
        if (offset_ != bytes_.size()) {
            throw std::runtime_error("trailing bytes in transcript event payload");
        }
    }

private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - offset_) {
            throw std::runtime_error("truncated transcript event payload");
        }
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] std::uint32_t crc32c(const std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::byte item : bytes) {
        crc ^= std::to_integer<std::uint8_t>(item);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
    return ~crc;
}

void require_strict_sequence(
    const std::uint64_t previous,
    const std::uint64_t current,
    const bool first) {
    if (current == 0U || (!first && current <= previous)) {
        throw std::invalid_argument(
            "transcript ingress sequences must be positive and strictly increasing");
    }
}

void require_core_contiguous_sequence(
    const std::uint64_t last_applied,
    const std::span<const CoreEvent> events) {
    std::uint64_t previous = last_applied;
    for (const CoreEvent& event : events) {
        if (previous == std::numeric_limits<std::uint64_t>::max() ||
            event.ingress_sequence != previous + 1U) {
            throw std::invalid_argument(
                "core transcript sequences must be contiguous with core state");
        }
        previous = event.ingress_sequence;
    }
}

void put_conversion(Writer& writer, const RationalConversion conversion) {
    writer.u64(conversion.numerator());
    writer.u64(conversion.denominator());
}

[[nodiscard]] RationalConversion read_conversion(Reader& reader) {
    const std::uint64_t numerator = reader.u64();
    const std::uint64_t denominator = reader.u64();
    const auto conversion = RationalConversion::create(numerator, denominator);
    if (!conversion.has_value()) {
        throw std::runtime_error("invalid rational conversion in transcript");
    }
    return *conversion;
}

void put_parent_order(Writer& writer, const ParentOrder& order) {
    writer.u64(order.id.value());
    writer.u32(order.instrument.value());
    writer.enum8(order.side, Side::Sell, "side");
    writer.u64(order.quantity.units);
    writer.i64(order.limit_price.units);
    writer.enum8(order.intent, RoutingIntent::PassiveGTC, "routing intent");
}

[[nodiscard]] ParentOrder read_parent_order(Reader& reader) {
    ParentOrder order;
    order.id = ParentOrderId{reader.u64()};
    order.instrument = InstrumentId{reader.u32()};
    order.side = reader.enum8(Side::Sell, "side");
    order.quantity = Quantity{reader.u64()};
    order.limit_price = Price{reader.i64()};
    order.intent = reader.enum8(RoutingIntent::PassiveGTC, "routing intent");
    return order;
}

void put_parent_replace(Writer& writer, const ParentReplace& replacement) {
    writer.u64(replacement.id.value());
    writer.u64(replacement.new_total_quantity.units);
    writer.i64(replacement.new_limit_price.units);
}

[[nodiscard]] ParentReplace read_parent_replace(Reader& reader) {
    return ParentReplace{
        ParentOrderId{reader.u64()},
        Quantity{reader.u64()},
        Price{reader.i64()},
    };
}

void put_level(Writer& writer, const md::Level& level) {
    writer.i64(level.price.units);
    writer.u64(level.quantity.units);
    writer.u32(level.order_count);
}

[[nodiscard]] md::Level read_level(Reader& reader) {
    return md::Level{
        Price{reader.i64()}, Quantity{reader.u64()}, reader.u32()};
}

void put_book(Writer& writer, const md::BookView& book) {
    if (book.bid_count > md::kMaxDepthLevels ||
        book.ask_count > md::kMaxDepthLevels) {
        throw std::invalid_argument("invalid book depth count in transcript");
    }
    writer.u32(book.venue.value());
    writer.u32(book.channel.value());
    writer.u32(book.profile.value());
    writer.u32(book.instrument.value());
    writer.u16(book.bid_count);
    for (std::size_t index = 0U; index < book.bid_count; ++index) {
        put_level(writer, book.bids[index]);
    }
    writer.u16(book.ask_count);
    for (std::size_t index = 0U; index < book.ask_count; ++index) {
        put_level(writer, book.asks[index]);
    }
    writer.u64(book.feed_session_epoch);
    writer.u64(book.recovery_generation);
    writer.u64(book.view_generation);
    writer.u64(book.last_applied_sequence);
    writer.u64(book.next_sequence);
    writer.i64(book.exchange_timestamp_ns);
    writer.u64(book.local_receive_time_ns);
    writer.u64(book.local_publication_time_ns);
    writer.enum8(
        book.feed_state, MarketDataState::Failed, "market-data state");
    writer.enum8(
        book.trading_state, InstrumentState::Closed, "instrument state");
    writer.boolean(book.depth_truncated);
}

[[nodiscard]] md::BookView read_book(Reader& reader) {
    md::BookView book;
    book.venue = VenueId{reader.u32()};
    book.channel = ChannelId{reader.u32()};
    book.profile = ProfileId{reader.u32()};
    book.instrument = InstrumentId{reader.u32()};
    book.bid_count = reader.u16();
    if (book.bid_count > md::kMaxDepthLevels) {
        throw std::runtime_error("invalid bid depth count in transcript");
    }
    for (std::size_t index = 0U; index < book.bid_count; ++index) {
        book.bids[index] = read_level(reader);
    }
    book.ask_count = reader.u16();
    if (book.ask_count > md::kMaxDepthLevels) {
        throw std::runtime_error("invalid ask depth count in transcript");
    }
    for (std::size_t index = 0U; index < book.ask_count; ++index) {
        book.asks[index] = read_level(reader);
    }
    book.feed_session_epoch = reader.u64();
    book.recovery_generation = reader.u64();
    book.view_generation = reader.u64();
    book.last_applied_sequence = reader.u64();
    book.next_sequence = reader.u64();
    book.exchange_timestamp_ns = reader.i64();
    book.local_receive_time_ns = reader.u64();
    book.local_publication_time_ns = reader.u64();
    book.feed_state = reader.enum8(
        MarketDataState::Failed, "market-data state");
    book.trading_state = reader.enum8(
        InstrumentState::Closed, "instrument state");
    book.depth_truncated = reader.boolean();
    return book;
}

void put_candidate_table(Writer& writer, const CandidateTable& table) {
    writer.u64(table.generation());
    writer.u64(table.source_rib_revision());
    writer.u32(checked_u32(table.entries().size(), "candidate entry count"));
    for (const CandidateSet& entry : table.entries()) {
        writer.u32(entry.instrument.value());
        writer.string(entry.canonical_symbol);
        writer.u32(checked_u32(entry.candidates.size(), "candidate count"));
        for (const CandidateHandle& candidate : entry.candidates) {
            writer.u32(candidate.venue.value());
            writer.string(candidate.venue_name);
            writer.string(candidate.wire_symbol);
            put_conversion(writer, candidate.price_conversion);
            put_conversion(writer, candidate.quantity_conversion);
            writer.boolean(candidate.capabilities.aggressive_ioc);
            writer.boolean(candidate.capabilities.passive_gtc);
            writer.u64(candidate.allocation_cap.units);
            writer.u32(candidate.aggressive_priority);
            writer.u32(candidate.passive_priority);
            writer.u64(candidate.configured_latency_ns);
            writer.enum8(candidate.admin_state, AdminState::Down, "admin state");
        }
    }
}

[[nodiscard]] std::shared_ptr<const CandidateTable> read_candidate_table(
    Reader& reader) {
    const std::uint64_t generation = reader.u64();
    const std::uint64_t source_revision = reader.u64();
    const std::uint32_t entry_count = reader.count("candidate entry count");
    if (entry_count == 0U) {
        throw std::runtime_error("empty candidate table in transcript");
    }

    std::vector<RouteConfig> routes;
    for (std::uint32_t entry_index = 0U;
         entry_index < entry_count;
         ++entry_index) {
        const InstrumentId instrument{reader.u32()};
        const std::string canonical_symbol = reader.string();
        const std::uint32_t candidate_count = reader.count("candidate count");
        if (candidate_count == 0U ||
            candidate_count > maximum_collection_size - routes.size()) {
            throw std::runtime_error("invalid candidate count in transcript");
        }
        routes.reserve(routes.size() + candidate_count);
        for (std::uint32_t candidate_index = 0U;
             candidate_index < candidate_count;
             ++candidate_index) {
            RouteConfig route;
            route.venue = VenueId{reader.u32()};
            route.instrument = instrument;
            route.venue_name = reader.string();
            route.canonical_symbol = canonical_symbol;
            route.wire_symbol = reader.string();
            route.price_conversion = read_conversion(reader);
            route.quantity_conversion = read_conversion(reader);
            route.capabilities.aggressive_ioc = reader.boolean();
            route.capabilities.passive_gtc = reader.boolean();
            route.allocation_cap = Quantity{reader.u64()};
            route.aggressive_priority = reader.u32();
            route.passive_priority = reader.u32();
            route.configured_latency_ns = reader.u64();
            route.admin_state = reader.enum8(AdminState::Down, "admin state");
            routes.push_back(std::move(route));
        }
    }

    RouteRib rib;
    std::vector<RouteValidationError> errors;
    if (!rib.replace_all(std::move(routes), errors) ||
        !rib.restore_revision(source_revision)) {
        throw std::runtime_error("invalid candidate table in transcript");
    }
    try {
        FibCompiler compiler;
        return compiler.compile_at_generation(rib, generation);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid candidate generation in transcript");
    }
}

void put_runtime_view(Writer& writer, const CandidateRuntimeView& view) {
    writer.u32(view.venue.value());
    writer.u64(view.adjacency_generation);
    writer.boolean(view.book.has_value());
    if (view.book.has_value()) {
        put_book(writer, *view.book);
    }
    writer.enum8(
        view.market_data_state,
        MarketDataState::Failed,
        "market-data state");
    writer.enum8(
        view.order_entry_state,
        OrderEntryState::Degraded,
        "order-entry state");
    writer.enum8(view.egress_state, EgressState::Congested, "egress state");
    writer.enum8(
        view.instrument_state,
        InstrumentState::Closed,
        "instrument state");
    writer.boolean(view.feed_sufficiently_fresh);
    writer.boolean(view.conversion_compatible);
    writer.boolean(view.risk_allowed);
}

[[nodiscard]] CandidateRuntimeView read_runtime_view(Reader& reader) {
    CandidateRuntimeView view;
    view.venue = VenueId{reader.u32()};
    view.adjacency_generation = reader.u64();
    if (reader.boolean()) {
        view.book = read_book(reader);
    }
    view.market_data_state = reader.enum8(
        MarketDataState::Failed, "market-data state");
    view.order_entry_state = reader.enum8(
        OrderEntryState::Degraded, "order-entry state");
    view.egress_state = reader.enum8(
        EgressState::Congested, "egress state");
    view.instrument_state = reader.enum8(
        InstrumentState::Closed, "instrument state");
    view.feed_sufficiently_fresh = reader.boolean();
    view.conversion_compatible = reader.boolean();
    view.risk_allowed = reader.boolean();
    return view;
}

void put_local_dispatch(
    Writer& writer,
    const adapters::LocalDispatchResult& result) {
    writer.u64(result.child.value());
    writer.enum8(
        result.state,
        adapters::LocalDispatchState::Unknown,
        "local dispatch state");
    writer.string(result.detail);
    writer.enum8(
        result.kind,
        adapters::DispatchAttemptKind::Cancel,
        "dispatch attempt kind");
    writer.u64(result.attempt_id);
    writer.u32(result.venue.value());
    writer.u64(result.session_epoch);
    writer.string(result.wire_order_id);
}

[[nodiscard]] adapters::LocalDispatchResult read_local_dispatch(Reader& reader) {
    adapters::LocalDispatchResult result;
    result.child = ChildOrderId{reader.u64()};
    result.state = reader.enum8(
        adapters::LocalDispatchState::Unknown, "local dispatch state");
    result.detail = reader.string();
    result.kind = reader.enum8(
        adapters::DispatchAttemptKind::Cancel, "dispatch attempt kind");
    result.attempt_id = reader.u64();
    result.venue = VenueId{reader.u32()};
    result.session_epoch = reader.u64();
    result.wire_order_id = reader.string();
    return result;
}

void put_execution_report(
    Writer& writer,
    const adapters::NormalizedExecutionReport& report) {
    writer.u32(report.venue.value());
    writer.u64(report.session_epoch);
    writer.string(report.wire_order_id);
    writer.string(report.execution_id);
    writer.string(report.durable_report_key);
    writer.enum8(
        report.kind, adapters::ExecutionKind::CancelRejected, "execution kind");
    writer.enum8(
        report.status,
        adapters::NormalizedOrderStatus::Rejected,
        "normalized order status");
    writer.boolean(report.last_quantity.has_value());
    if (report.last_quantity.has_value()) {
        writer.u64(report.last_quantity->units);
    }
    writer.boolean(report.last_price.has_value());
    if (report.last_price.has_value()) {
        writer.i64(report.last_price->units);
    }
    writer.u64(report.cumulative_quantity.units);
    writer.boolean(report.leaves_quantity.has_value());
    if (report.leaves_quantity.has_value()) {
        writer.u64(report.leaves_quantity->units);
    }
    writer.string(report.reject_reason);
    writer.enum8(
        report.value_domain,
        adapters::ExecutionValueDomain::Venue,
        "execution value domain");
}

[[nodiscard]] adapters::NormalizedExecutionReport read_execution_report(
    Reader& reader) {
    adapters::NormalizedExecutionReport report;
    report.venue = VenueId{reader.u32()};
    report.session_epoch = reader.u64();
    report.wire_order_id = reader.string();
    report.execution_id = reader.string();
    report.durable_report_key = reader.string();
    report.kind = reader.enum8(
        adapters::ExecutionKind::CancelRejected, "execution kind");
    report.status = reader.enum8(
        adapters::NormalizedOrderStatus::Rejected,
        "normalized order status");
    if (reader.boolean()) {
        report.last_quantity = Quantity{reader.u64()};
    }
    if (reader.boolean()) {
        report.last_price = Price{reader.i64()};
    }
    report.cumulative_quantity = Quantity{reader.u64()};
    if (reader.boolean()) {
        report.leaves_quantity = Quantity{reader.u64()};
    }
    report.reject_reason = reader.string();
    report.value_domain = reader.enum8(
        adapters::ExecutionValueDomain::Venue, "execution value domain");
    return report;
}

[[nodiscard]] std::pair<CoreTranscriptEventKind, Bytes> encode_core_event(
    const CoreEvent& event) {
    Writer writer;
    switch (event.payload.index()) {
        case 0U:
            put_parent_order(writer, std::get<ParentSubmitEvent>(event.payload).order);
            return {CoreTranscriptEventKind::ParentSubmit, std::move(writer).take()};
        case 1U:
            writer.u64(std::get<ParentCancelEvent>(event.payload).parent.value());
            return {CoreTranscriptEventKind::ParentCancel, std::move(writer).take()};
        case 2U:
            put_parent_replace(
                writer, std::get<ParentReplaceEvent>(event.payload).replacement);
            return {CoreTranscriptEventKind::ParentReplace, std::move(writer).take()};
        case 3U: {
            const auto& table =
                std::get<CandidateTablePublishedEvent>(event.payload).table;
            if (!table) {
                throw std::invalid_argument("null candidate table in transcript");
            }
            put_candidate_table(writer, *table);
            return {
                CoreTranscriptEventKind::CandidateTablePublished,
                std::move(writer).take(),
            };
        }
        case 4U: {
            const auto& published =
                std::get<RuntimeViewPublishedEvent>(event.payload);
            writer.u32(published.instrument.value());
            put_runtime_view(writer, published.view);
            return {
                CoreTranscriptEventKind::RuntimeViewPublished,
                std::move(writer).take(),
            };
        }
        case 5U: {
            const auto& session = std::get<OrderEntrySessionEvent>(event.payload);
            writer.u32(session.venue.value());
            writer.u64(session.session_epoch);
            writer.enum8(
                session.state,
                OrderEntryState::Degraded,
                "order-entry state");
            return {
                CoreTranscriptEventKind::OrderEntrySession,
                std::move(writer).take(),
            };
        }
        case 6U: {
            const auto& started = std::get<DispatchStartedEvent>(event.payload);
            writer.u64(started.child.value());
            writer.enum8(
                started.kind,
                adapters::DispatchAttemptKind::Cancel,
                "dispatch attempt kind");
            writer.u64(started.attempt_id);
            writer.u32(started.venue.value());
            writer.u64(started.session_epoch);
            writer.string(started.wire_order_id);
            return {
                CoreTranscriptEventKind::DispatchStarted,
                std::move(writer).take(),
            };
        }
        case 7U:
            put_local_dispatch(
                writer, std::get<LocalDispatchEvent>(event.payload).result);
            return {CoreTranscriptEventKind::LocalDispatch, std::move(writer).take()};
        case 8U:
            put_execution_report(
                writer, std::get<ExecutionReportEvent>(event.payload).report);
            return {
                CoreTranscriptEventKind::ExecutionReport,
                std::move(writer).take(),
            };
        case 9U: {
            const auto& timeout = std::get<DispatchTimeoutEvent>(event.payload);
            writer.u64(timeout.child.value());
            writer.string(timeout.reason);
            return {
                CoreTranscriptEventKind::DispatchTimeout,
                std::move(writer).take(),
            };
        }
        case 10U: {
            const auto& kill = std::get<KillSwitchEvent>(event.payload);
            writer.boolean(kill.active);
            writer.string(kill.reason);
            return {CoreTranscriptEventKind::KillSwitch, std::move(writer).take()};
        }
        case 11U:
            writer.boolean(std::get<RoutingGateEvent>(event.payload).enabled);
            return {CoreTranscriptEventKind::RoutingGate, std::move(writer).take()};
        case 12U: {
            const auto& reconciliation =
                std::get<ReconciliationEvent>(event.payload);
            writer.u64(reconciliation.child.value());
            writer.enum8(
                reconciliation.disposition,
                ReconciliationDisposition::Unresolved,
                "reconciliation disposition");
            writer.u64(reconciliation.cumulative_quantity.units);
            return {
                CoreTranscriptEventKind::Reconciliation,
                std::move(writer).take(),
            };
        }
        default:
            throw std::invalid_argument("unknown CoreEvent variant in transcript");
    }
}

[[nodiscard]] CoreEvent decode_core_event(const TranscriptEvent& frame) {
    Reader reader{frame.payload};
    CoreEvent event;
    event.ingress_sequence = frame.ingress_sequence;
    switch (static_cast<CoreTranscriptEventKind>(frame.kind)) {
        case CoreTranscriptEventKind::ParentSubmit:
            event.payload = ParentSubmitEvent{read_parent_order(reader)};
            break;
        case CoreTranscriptEventKind::ParentCancel:
            event.payload = ParentCancelEvent{ParentOrderId{reader.u64()}};
            break;
        case CoreTranscriptEventKind::ParentReplace:
            event.payload = ParentReplaceEvent{read_parent_replace(reader)};
            break;
        case CoreTranscriptEventKind::CandidateTablePublished:
            event.payload = CandidateTablePublishedEvent{
                read_candidate_table(reader)};
            break;
        case CoreTranscriptEventKind::RuntimeViewPublished: {
            const InstrumentId instrument{reader.u32()};
            event.payload = RuntimeViewPublishedEvent{
                instrument, read_runtime_view(reader)};
            break;
        }
        case CoreTranscriptEventKind::OrderEntrySession: {
            OrderEntrySessionEvent session;
            session.venue = VenueId{reader.u32()};
            session.session_epoch = reader.u64();
            session.state = reader.enum8(
                OrderEntryState::Degraded, "order-entry state");
            event.payload = session;
            break;
        }
        case CoreTranscriptEventKind::DispatchStarted: {
            DispatchStartedEvent started;
            started.child = ChildOrderId{reader.u64()};
            started.kind = reader.enum8(
                adapters::DispatchAttemptKind::Cancel,
                "dispatch attempt kind");
            started.attempt_id = reader.u64();
            started.venue = VenueId{reader.u32()};
            started.session_epoch = reader.u64();
            started.wire_order_id = reader.string();
            event.payload = std::move(started);
            break;
        }
        case CoreTranscriptEventKind::LocalDispatch:
            event.payload = LocalDispatchEvent{read_local_dispatch(reader)};
            break;
        case CoreTranscriptEventKind::ExecutionReport:
            event.payload = ExecutionReportEvent{read_execution_report(reader)};
            break;
        case CoreTranscriptEventKind::DispatchTimeout: {
            DispatchTimeoutEvent timeout;
            timeout.child = ChildOrderId{reader.u64()};
            timeout.reason = reader.string();
            event.payload = std::move(timeout);
            break;
        }
        case CoreTranscriptEventKind::KillSwitch: {
            KillSwitchEvent kill;
            kill.active = reader.boolean();
            kill.reason = reader.string();
            event.payload = std::move(kill);
            break;
        }
        case CoreTranscriptEventKind::RoutingGate:
            event.payload = RoutingGateEvent{reader.boolean()};
            break;
        case CoreTranscriptEventKind::Reconciliation: {
            ReconciliationEvent reconciliation;
            reconciliation.child = ChildOrderId{reader.u64()};
            reconciliation.disposition = reader.enum8(
                ReconciliationDisposition::Unresolved,
                "reconciliation disposition");
            reconciliation.cumulative_quantity = Quantity{reader.u64()};
            event.payload = reconciliation;
            break;
        }
        default:
            throw std::runtime_error("unknown core transcript event kind");
    }
    reader.require_end();
    return event;
}

void put_new_intent(Writer& writer, const adapters::NewOrderIntent& intent) {
    writer.u32(intent.venue.value());
    writer.u64(intent.parent.value());
    writer.u64(intent.child.value());
    writer.u32(intent.instrument.value());
    writer.enum8(intent.side, Side::Sell, "side");
    writer.u64(intent.quantity.units);
    writer.i64(intent.limit_price.units);
    writer.enum8(
        intent.time_in_force,
        adapters::TimeInForce::GoodTillCancel,
        "time in force");
    writer.u64(intent.parent_revision);
    writer.u64(intent.session_epoch);
    writer.string(intent.wire_order_id);
    writer.string(intent.wire_symbol);
    writer.u64(intent.dispatch_attempt_id);
    writer.u64(intent.canonical_quantity.units);
    writer.i64(intent.canonical_limit_price.units);
    writer.u64(intent.venue_quantity.units);
    writer.i64(intent.venue_limit_price.units);
}

void put_cancel_intent(
    Writer& writer,
    const adapters::CancelOrderIntent& intent) {
    writer.u32(intent.venue.value());
    writer.u64(intent.parent.value());
    writer.u64(intent.child.value());
    writer.enum8(intent.side, Side::Sell, "side");
    writer.u64(intent.original_quantity.units);
    writer.u64(intent.session_epoch);
    writer.string(intent.original_wire_order_id);
    writer.string(intent.cancel_wire_order_id);
    writer.string(intent.wire_symbol);
    writer.u64(intent.dispatch_attempt_id);
    writer.u64(intent.canonical_original_quantity.units);
    writer.u64(intent.venue_original_quantity.units);
}

[[nodiscard]] Bytes encode_replay_step(
    const ApplyResult& result,
    const std::span<const std::byte> state) {
    Writer writer;
    writer.boolean(result.command_accepted);
    writer.boolean(result.idempotent_replay);
    writer.enum8(
        result.command_error,
        CommandError::RecoveryInProgress,
        "command error");

    writer.u32(checked_u32(result.records.size(), "deterministic record count"));
    for (const DeterministicRecord& record : result.records) {
        const auto raw_kind = static_cast<std::uint16_t>(record.kind);
        if (raw_kind < static_cast<std::uint16_t>(
                           DeterministicRecordKind::ParentSubmit) ||
            raw_kind > static_cast<std::uint16_t>(
                           DeterministicRecordKind::ExecutionCanonicalized)) {
            throw std::runtime_error("invalid deterministic record kind");
        }
        writer.u16(raw_kind);
        writer.u64(record.ingress_sequence);
        writer.blob(record.payload);
    }

    writer.u32(checked_u32(result.effects.size(), "core effect count"));
    for (const CoreEffect& effect : result.effects) {
        if (std::holds_alternative<DispatchNewEffect>(effect)) {
            writer.u8(1U);
            put_new_intent(writer, std::get<DispatchNewEffect>(effect).intent);
        } else if (std::holds_alternative<DispatchCancelEffect>(effect)) {
            writer.u8(2U);
            put_cancel_intent(writer, std::get<DispatchCancelEffect>(effect).intent);
        } else {
            throw std::runtime_error("unknown core effect variant");
        }
    }

    writer.u32(checked_u32(result.reports.size(), "parent report count"));
    for (const ParentReport& report : result.reports) {
        writer.u64(report.parent.value());
        writer.enum8(
            report.kind, ParentReportKind::Suspended, "parent report kind");
        writer.enum8(report.state, ParentState::Rejected, "parent state");
        writer.u64(report.cumulative_filled.units);
        writer.u64(report.explicit_remainder.units);
        writer.string(report.reason);
    }
    writer.blob(state);
    return std::move(writer).take();
}

[[nodiscard]] CoreReplayImage replay_decoded(
    SorCore& core,
    const std::span<const CoreEvent> events) {
    std::vector<TranscriptEvent> outcomes;
    outcomes.reserve(events.size());
    for (const CoreEvent& event : events) {
        const auto [kind, ignored_payload] = encode_core_event(event);
        static_cast<void>(ignored_payload);
        const ApplyResult result = core.apply(event);
        const Bytes state = core.save_state();
        outcomes.push_back(TranscriptEvent{
            event.ingress_sequence,
            static_cast<std::uint16_t>(kind),
            encode_replay_step(result, state),
        });
    }
    CoreReplayImage image;
    image.outcome_transcript = TranscriptCodec::encode(outcomes);
    image.final_state = core.save_state();
    return image;
}

}  // namespace

std::vector<std::byte> TranscriptCodec::encode(
    const std::span<const TranscriptEvent> events) {
    if (events.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("too many transcript events");
    }

    Writer output;
    for (const std::byte item : magic) {
        output.u8(std::to_integer<std::uint8_t>(item));
    }
    output.u16(schema_version);
    output.u32(static_cast<std::uint32_t>(events.size()));

    std::uint64_t previous = 0U;
    bool first = true;
    for (const auto& event : events) {
        require_strict_sequence(previous, event.ingress_sequence, first);
        first = false;
        previous = event.ingress_sequence;
        if (event.kind == 0U) {
            throw std::invalid_argument("transcript event kind must be nonzero");
        }

        const std::size_t frame_start = output.bytes().size();
        output.u64(event.ingress_sequence);
        output.u16(event.kind);
        output.u32(checked_u32(event.payload.size(), "transcript event payload"));
        for (const std::byte item : event.payload) {
            output.u8(std::to_integer<std::uint8_t>(item));
        }
        output.u32(crc32c(std::span<const std::byte>{output.bytes()}.subspan(
            frame_start)));
    }
    return std::move(output).take();
}

std::vector<TranscriptEvent> TranscriptCodec::decode(
    const std::span<const std::byte> bytes) {
    if (bytes.size() < transcript_header_size ||
        !std::equal(magic.begin(), magic.end(), bytes.begin())) {
        throw std::runtime_error("invalid transcript header");
    }

    Reader reader{bytes.subspan(magic.size())};
    if (reader.u16() != schema_version) {
        throw std::runtime_error("unsupported transcript schema");
    }
    const std::uint32_t count = reader.u32();
    if (count > (bytes.size() - transcript_header_size) / minimum_frame_size) {
        throw std::runtime_error("invalid transcript event count");
    }

    // Frame CRCs cover their exact bytes, so use an offset over the original
    // image while retaining Reader's checked integer decoding.
    std::size_t offset = transcript_header_size;
    std::vector<TranscriptEvent> events;
    events.reserve(count);
    std::uint64_t previous = 0U;
    bool first = true;
    for (std::uint32_t index = 0U; index < count; ++index) {
        const std::size_t frame_start = offset;
        Reader frame_header{bytes.subspan(offset)};
        TranscriptEvent event;
        event.ingress_sequence = frame_header.u64();
        event.kind = frame_header.u16();
        const std::uint32_t payload_size = frame_header.u32();
        constexpr std::size_t frame_header_size = 14U;
        if (payload_size > bytes.size() - offset - frame_header_size ||
            bytes.size() - offset - frame_header_size - payload_size < 4U) {
            throw std::runtime_error("truncated transcript frame");
        }
        offset += frame_header_size;
        event.payload.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
        offset += payload_size;
        Reader checksum_reader{bytes.subspan(offset)};
        const std::uint32_t stored_crc = checksum_reader.u32();
        const std::uint32_t calculated_crc =
            crc32c(bytes.subspan(frame_start, offset - frame_start));
        offset += 4U;
        if (stored_crc != calculated_crc) {
            throw std::runtime_error("transcript checksum mismatch");
        }
        require_strict_sequence(previous, event.ingress_sequence, first);
        first = false;
        previous = event.ingress_sequence;
        if (event.kind == 0U) {
            throw std::runtime_error("invalid transcript event kind");
        }
        events.push_back(std::move(event));
    }
    if (offset != bytes.size()) {
        throw std::runtime_error("trailing bytes after transcript");
    }
    return events;
}

std::vector<std::byte> TranscriptReplay::run(
    const std::span<const TranscriptEvent> input,
    const Apply& apply) {
    if (!apply) {
        throw std::invalid_argument("transcript replay callback is empty");
    }
    std::vector<TranscriptEvent> outcomes;
    outcomes.reserve(input.size());
    for (const auto& event : input) {
        outcomes.push_back(TranscriptEvent{
            event.ingress_sequence,
            event.kind,
            apply(event),
        });
    }
    return TranscriptCodec::encode(outcomes);
}

std::vector<std::byte> CoreEventTranscriptCodec::encode(
    const std::span<const CoreEvent> events) {
    std::vector<TranscriptEvent> frames;
    frames.reserve(events.size());
    for (const CoreEvent& event : events) {
        auto [kind, payload] = encode_core_event(event);
        frames.push_back(TranscriptEvent{
            event.ingress_sequence,
            static_cast<std::uint16_t>(kind),
            std::move(payload),
        });
    }
    return TranscriptCodec::encode(frames);
}

std::vector<CoreEvent> CoreEventTranscriptCodec::decode(
    const std::span<const std::byte> bytes) {
    const std::vector<TranscriptEvent> frames = TranscriptCodec::decode(bytes);
    std::vector<CoreEvent> events;
    events.reserve(frames.size());
    for (const TranscriptEvent& frame : frames) {
        CoreEvent event = decode_core_event(frame);
        const auto [kind, canonical_payload] = encode_core_event(event);
        if (static_cast<std::uint16_t>(kind) != frame.kind ||
            canonical_payload != frame.payload) {
            throw std::runtime_error("non-canonical core transcript event");
        }
        events.push_back(std::move(event));
    }
    return events;
}

CoreReplayImage CoreTranscriptReplay::run(
    SorCore& core,
    const std::span<const CoreEvent> events) {
    // Encode/decode before mutation: this validates ordering, all enums, all
    // lengths, and reconstructed candidate tables as one atomic preflight.
    const Bytes encoded = CoreEventTranscriptCodec::encode(events);
    const std::vector<CoreEvent> validated =
        CoreEventTranscriptCodec::decode(encoded);
    require_core_contiguous_sequence(
        core.snapshot().last_ingress_sequence, validated);
    return replay_decoded(core, validated);
}

CoreReplayImage CoreTranscriptReplay::run_encoded(
    SorCore& core,
    const std::span<const std::byte> transcript) {
    const std::vector<CoreEvent> events =
        CoreEventTranscriptCodec::decode(transcript);
    require_core_contiguous_sequence(
        core.snapshot().last_ingress_sequence, events);
    return replay_decoded(core, events);
}

}  // namespace sorx


