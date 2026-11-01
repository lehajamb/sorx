#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/marketdata/feed_arbiter.hpp"
#include "sorx/marketdata/fixed_l3_book.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> count{0U};

void record() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t size) {
    record();
    if (void* const result = std::malloc(size == 0U ? 1U : size)) {
        return result;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t size,
    const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* const result =
            _aligned_malloc(size == 0U ? 1U : size, alignment)) {
        return result;
    }
#else
    const std::size_t nonzero = size == 0U ? 1U : size;
    const std::size_t rounded =
        ((nonzero + alignment - 1U) / alignment) * alignment;
    if (void* const result = std::aligned_alloc(alignment, rounded)) {
        return result;
    }
#endif
    throw std::bad_alloc{};
}

void deallocate_aligned(void* const memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

class Scope final {
public:
    Scope() noexcept {
        count.store(0U, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    [[nodiscard]] std::size_t allocations() const noexcept {
        return count.load(std::memory_order_relaxed);
    }
};

}  // namespace allocation_probe

void* operator new(const std::size_t size) {
    return allocation_probe::allocate(size);
}
void* operator new[](const std::size_t size) {
    return allocation_probe::allocate(size);
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void* operator new(
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete(
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}

namespace {

using namespace sorx;
using namespace sorx::marketdata;

[[nodiscard]] FixedL3BookConfig config(
    const std::size_t orders = 8U,
    const std::size_t index = 8U,
    const std::size_t levels = 8U,
    const std::size_t events = 8U) noexcept {
    return FixedL3BookConfig{VenueId{1U},
                             ChannelId{2U},
                             ProfileId{3U},
                             InstrumentId{4U},
                             orders,
                             index,
                             levels,
                             events,
                             InstrumentState::Trading};
}

[[nodiscard]] BookPacketContext context(
    const std::uint64_t last_sequence) noexcept {
    return BookPacketContext{1U, 1U, last_sequence, 1'000U + last_sequence,
                             1'001U + last_sequence,
                             MarketDataState::Live};
}

[[nodiscard]] L3Event add(
    const OrderReference reference,
    const Side side,
    const std::int64_t price,
    const std::uint64_t quantity) noexcept {
    return L3Event{L3Operation::Add, reference, side, Price{price},
                   Quantity{quantity}, 100};
}

[[nodiscard]] L3Event modify(
    const OrderReference reference,
    const Side side,
    const std::int64_t price,
    const std::uint64_t quantity) noexcept {
    return L3Event{L3Operation::Modify, reference, side, Price{price},
                   Quantity{quantity}, 101};
}

[[nodiscard]] L3Event remove(const OrderReference reference) noexcept {
    return L3Event{L3Operation::Delete, reference, Side::Buy, Price{},
                   Quantity{}, 102};
}

void test_add_modify_delete_and_fifo() {
    FixedL3Book book{config()};
    const std::array initial{add(1U, Side::Buy, 100, 5U),
                             add(2U, Side::Buy, 100, 7U)};
    const BookApplyResult added = book.apply_packet(context(2U), initial);
    assert(added.status == BookApplyStatus::Applied);
    assert(added.publications.size() == 1U);
    assert(added.publications[0].valid());
    assert(added.publications[0].bid_count == 1U);
    assert(added.publications[0].bids[0].quantity == Quantity{12U});
    assert(added.publications[0].bids[0].order_count == 2U);

    std::array<OrderReference, 4U> fifo{};
    assert(book.copy_fifo(Side::Buy, Price{100}, fifo) == 2U);
    assert(fifo[0] == 1U && fifo[1] == 2U);

    const std::array quantity_change{modify(1U, Side::Buy, 100, 6U)};
    assert(book.apply_packet(context(3U), quantity_change).accepted());
    assert(book.copy_fifo(Side::Buy, Price{100}, fifo) == 2U);
    assert(fifo[0] == 1U && fifo[1] == 2U);
    assert(book.level(Side::Buy, Price{100})->aggregate_quantity ==
           Quantity{13U});

    const std::array move_away{modify(1U, Side::Buy, 99, 6U)};
    assert(book.apply_packet(context(4U), move_away).accepted());
    assert(book.copy_fifo(Side::Buy, Price{100}, fifo) == 1U);
    assert(fifo[0] == 2U);

    const std::array move_back{modify(1U, Side::Buy, 100, 6U)};
    assert(book.apply_packet(context(5U), move_back).accepted());
    assert(book.copy_fifo(Side::Buy, Price{100}, fifo) == 2U);
    assert(fifo[0] == 2U && fifo[1] == 1U);

    const std::array same{modify(1U, Side::Buy, 100, 6U)};
    const BookApplyResult unchanged = book.apply_packet(context(6U), same);
    assert(unchanged.status == BookApplyStatus::NoChange);
    assert(unchanged.publications.empty());

    const std::array deletes{remove(2U), remove(1U)};
    assert(book.apply_packet(context(8U), deletes).accepted());
    assert(!book.level(Side::Buy, Price{100}).has_value());
    assert(book.metadata().active_orders == 0U);
}

void test_packet_rollback_and_capacity() {
    FixedL3Book book{config()};
    const std::array first{add(1U, Side::Sell, 101, 5U)};
    assert(book.apply_packet(context(1U), first).accepted());
    const FixedL3BookMetadata before = book.metadata();

    const std::array duplicate_packet{add(2U, Side::Buy, 99, 3U),
                                      add(1U, Side::Buy, 98, 1U)};
    const BookApplyResult duplicate =
        book.apply_packet(context(3U), duplicate_packet);
    assert(duplicate.status == BookApplyStatus::DuplicateOrderReference);
    assert(!book.order(2U).has_value());
    assert(book.order(1U)->side == Side::Sell);
    assert(book.metadata().active_orders == before.active_orders);
    assert(book.metadata().view_generation == before.view_generation);

    const std::array crossed{add(3U, Side::Buy, 102, 1U)};
    assert(book.apply_packet(context(2U), crossed).status ==
           BookApplyStatus::CrossedBook);
    assert(!book.order(3U).has_value());

    FixedL3Book small{config(2U, 2U, 2U, 3U)};
    const std::array seed{add(10U, Side::Buy, 90, 1U)};
    assert(small.apply_packet(context(1U), seed).accepted());
    const std::array overflow{add(11U, Side::Buy, 89, 1U),
                              add(12U, Side::Buy, 88, 1U)};
    assert(small.apply_packet(context(3U), overflow).status ==
           BookApplyStatus::OrderCapacityExceeded);
    assert(!small.order(11U).has_value());
    assert(!small.order(12U).has_value());
    assert(small.metadata().active_orders == 1U);
}

void test_tombstone_reuse() {
    FixedL3Book book{config(2U, 2U, 2U, 2U)};
    const std::array one{add(1U, Side::Buy, 90, 1U)};
    assert(book.apply_packet(context(1U), one).accepted());
    const std::array deletion{remove(1U)};
    assert(book.apply_packet(context(2U), deletion).accepted());
    assert(book.metadata().index_tombstones == 1U);

    // The multiplicative hash is odd, so references 1 and 3 collide modulo 2.
    const std::array replacement{add(3U, Side::Buy, 91, 2U)};
    assert(book.apply_packet(context(3U), replacement).accepted());
    assert(book.order(3U).has_value());
    assert(book.metadata().occupied_index_entries == 1U);
    assert(book.metadata().index_tombstones == 0U);
}

void put_u64(std::byte* const destination, const std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        destination[index] = static_cast<std::byte>(
            (value >> static_cast<unsigned>((7U - index) * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::array<std::byte, 36U> encode(
    const L3Event& event) noexcept {
    std::array<std::byte, 36U> result{};
    result[0] = static_cast<std::byte>(event.operation);
    result[1] = static_cast<std::byte>(event.side == Side::Buy ? 0U : 1U);
    put_u64(result.data() + 4U, event.order_reference);
    put_u64(result.data() + 12U,
            std::bit_cast<std::uint64_t>(event.price.units));
    put_u64(result.data() + 20U, event.quantity.units);
    put_u64(result.data() + 28U,
            std::bit_cast<std::uint64_t>(event.exchange_timestamp_ns));
    return result;
}

void test_direct_registered_buffer_decode() {
    FixedL3Book book{config()};
    const auto wire = encode(add(44U, Side::Sell, 105, 9U));
    const BookApplyResult result = book.decode_and_apply(
        context(1U), wire, 1U, decode_canonical_l3_event);
    assert(result.accepted());
    assert(book.order(44U)->price == Price{105});

    std::array<std::byte, 37U> malformed{};
    const auto second = encode(add(45U, Side::Sell, 106, 1U));
    for (std::size_t index = 0U; index < second.size(); ++index) {
        malformed[index] = second[index];
    }
    malformed.back() = std::byte{0x01U};
    assert(book.decode_and_apply(context(2U), malformed, 1U,
                                 decode_canonical_l3_event)
               .status == BookApplyStatus::DecodeFailure);
    assert(!book.order(45U).has_value());
}

void test_ab_gap_failover_and_two_phase_commit() {
    FeedArbiter arbiter{{100U, 100U}};
    assert(arbiter.install_snapshot({FeedLeg::A, 1U, 10U, 1'000U, 1'000U}));
    assert(arbiter.inspect({FeedLeg::B, 1U, 10U, 0U, 1'010U, false})
               .action == ArbitrationAction::HeartbeatAccepted);

    assert(arbiter.inspect({FeedLeg::A, 1U, 11U, 1U, 1'020U, true})
               .action == ArbitrationAction::AwaitPeer);
    const ArbitrationResult rescued =
        arbiter.inspect({FeedLeg::B, 1U, 10U, 1U, 1'021U, true});
    assert(rescued.action == ArbitrationAction::FailoverApply);
    assert(rescued.should_apply());
    assert(arbiter.metadata().next_sequence == 10U);
    assert(arbiter.metadata().commit_pending);
    assert(!arbiter.may_publish(1'021U));
    assert(arbiter.commit(rescued.ticket, true) == CommitStatus::Committed);
    assert(arbiter.metadata().next_sequence == 11U);
    assert(arbiter.metadata().selected_leg == FeedLeg::B);
    assert(arbiter.inspect({FeedLeg::A, 1U, 10U, 1U, 1'022U, true})
               .action == ArbitrationAction::DuplicateIgnored);

    const ArbitrationResult next =
        arbiter.inspect({FeedLeg::B, 1U, 11U, 1U, 1'023U, true});
    assert(next.should_apply());
    assert(arbiter.commit(next.ticket, false) ==
           CommitStatus::ApplicationRejectedRecoveryRequired);
    assert(arbiter.metadata().state == FeedArbiterState::Recovering);
    assert(arbiter.metadata().next_sequence == 11U);
}

void test_gap_health_recovery_and_epoch_rollover() {
    FeedArbiter gap{{100U, 50U}};
    assert(gap.install_snapshot({FeedLeg::A, 1U, 20U, 1'000U, 1'000U}));
    assert(gap.inspect({FeedLeg::B, 1U, 20U, 0U, 1'010U, false})
               .action == ArbitrationAction::HeartbeatAccepted);
    assert(gap.inspect({FeedLeg::A, 1U, 22U, 1U, 1'020U, true})
               .action == ArbitrationAction::AwaitPeer);
    assert(gap.inspect({FeedLeg::B, 1U, 23U, 1U, 1'021U, true})
               .action == ArbitrationAction::RecoveryRequired);
    assert(gap.health(1'021U) == FeedHealth::Recovering);

    FeedArbiter single_gap{{100U, 50U}};
    assert(single_gap.install_snapshot(
        {FeedLeg::A, 1U, 20U, 1'000U, 1'000U}));
    assert(single_gap.inspect({FeedLeg::B, 1U, 20U, 0U, 1'010U, false})
               .action == ArbitrationAction::HeartbeatAccepted);
    assert(single_gap.inspect({FeedLeg::A, 1U, 22U, 1U, 1'020U, true})
               .action == ArbitrationAction::AwaitPeer);
    assert(single_gap.poll(1'051U) == FeedHealth::Recovering);

    FeedArbiter health{{100U, 50U}};
    assert(health.install_snapshot(
        {FeedLeg::A, 1U, 30U, 2'000U, 2'000U}));
    assert(health.health(2'049U) == FeedHealth::Healthy);
    assert(health.health(2'051U) == FeedHealth::QuoteStale);
    assert(!health.may_publish(2'051U));
    assert(health.health(2'101U) == FeedHealth::HeartbeatsStalled);

    const ArbitrationResult rollover =
        health.inspect({FeedLeg::B, 2U, 1U, 1U, 2'102U, true});
    assert(rollover.action == ArbitrationAction::EpochRollover);
    assert(health.metadata().feed_epoch == 2U);
    assert(health.metadata().state == FeedArbiterState::Recovering);
    assert(health.install_snapshot(
        {FeedLeg::B, 2U, 5U, 2'110U, 2'110U}));
    assert(health.metadata().next_sequence == 5U);
    assert(health.inspect({FeedLeg::A, 1U, 5U, 1U, 2'111U, true})
               .action == ArbitrationAction::StaleEpochIgnored);
}

void test_zero_post_construction_allocations() {
    FixedL3Book book{config()};
    FeedArbiter arbiter{{100U, 100U}};
    std::size_t allocations = 0U;
    {
        allocation_probe::Scope scope;
        const std::array events{add(70U, Side::Buy, 80, 3U),
                                add(71U, Side::Sell, 120, 4U)};
        assert(book.apply_packet(context(2U), events).accepted());
        assert(book.order(70U).has_value());
        std::array<OrderReference, 2U> fifo{};
        assert(book.copy_fifo(Side::Buy, Price{80}, fifo) == 1U);

        assert(arbiter.install_snapshot(
            {FeedLeg::A, 1U, 1U, 3'000U, 3'000U}));
        const ArbitrationResult decision =
            arbiter.inspect({FeedLeg::A, 1U, 1U, 2U, 3'001U, true});
        assert(decision.should_apply());
        assert(arbiter.commit(decision.ticket, true) ==
               CommitStatus::Committed);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
}

static_assert(noexcept(std::declval<FixedL3Book&>().apply_packet(
    BookPacketContext{}, std::span<const L3Event>{})));
static_assert(noexcept(std::declval<FeedArbiter&>().inspect(
    FeedPacketObservation{})));

}  // namespace

int main() {
    test_add_modify_delete_and_fifo();
    test_packet_rollback_and_capacity();
    test_tombstone_reuse();
    test_direct_registered_buffer_decode();
    test_ab_gap_failover_and_two_phase_commit();
    test_gap_health_recovery_and_epoch_rollover();
    test_zero_post_construction_allocations();
    return 0;
}
