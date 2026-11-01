#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/md/feed_engine.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using sorx::ChannelId;
using sorx::InstrumentId;
using sorx::InstrumentState;
using sorx::MarketDataState;
using sorx::Price;
using sorx::ProfileId;
using sorx::Quantity;
using sorx::Side;
using sorx::VenueId;
using sorx::md::AddOrderEvent;
using sorx::md::DeleteOrderEvent;
using sorx::md::EndOfSessionEvent;
using sorx::md::EngineUpdate;
using sorx::md::ExecuteOrderEvent;
using sorx::md::FeedEngine;
using sorx::md::FeedEngineConfig;
using sorx::md::FeedError;
using sorx::md::FeedEvent;
using sorx::md::InstrumentDirectoryEvent;
using sorx::md::KnownBookNeutralEvent;
using sorx::md::PacketFrame;
using sorx::md::RecoveryAction;
using sorx::md::SnapshotImage;
using sorx::md::TradingStateEvent;

[[nodiscard]] FeedEngine engine(
    const std::size_t depth = 32,
    const std::size_t packet_capacity = 16,
    const std::size_t event_capacity = 128) {
    return FeedEngine{FeedEngineConfig{
        VenueId{7},
        ChannelId{9},
        ProfileId{11},
        depth,
        sorx::md::RecoveryStrategy::FreshSnapshot,
        packet_capacity,
        event_capacity,
    }};
}

[[nodiscard]] std::vector<FeedEvent> baseline_snapshot_events() {
    return {
        InstrumentDirectoryEvent{17, InstrumentId{101}, 1},
        TradingStateEvent{17, InstrumentId{101}, InstrumentState::Trading, 2},
        AddOrderEvent{1, 17, InstrumentId{101}, Side::Buy, Quantity{5}, Price{100}, 3},
        AddOrderEvent{2, 17, InstrumentId{101}, Side::Buy, Quantity{7}, Price{100}, 4},
        AddOrderEvent{3, 17, InstrumentId{101}, Side::Sell, Quantity{9}, Price{105}, 5},
        KnownBookNeutralEvent{5},
    };
}

[[nodiscard]] EngineUpdate apply_snapshot(
    FeedEngine& value,
    const std::vector<FeedEvent>& events,
    const std::uint64_t session,
    const std::uint64_t cut,
    const std::uint64_t high_water,
    const std::uint64_t receive_time,
    const std::uint64_t publication_time) {
    return value.apply_snapshot(SnapshotImage{
        events,
        cut,
        receive_time,
        publication_time,
        session,
        high_water,
    });
}

[[nodiscard]] EngineUpdate apply_packet(
    FeedEngine& value,
    const std::uint64_t session,
    const std::uint64_t first_sequence,
    const std::vector<FeedEvent>& events,
    const std::uint64_t receive_time,
    const std::uint64_t publication_time) {
    assert(events.size() < std::numeric_limits<std::uint16_t>::max());
    return value.apply_packet(PacketFrame{
        session,
        first_sequence,
        static_cast<std::uint16_t>(events.size()),
        events,
        receive_time,
        publication_time,
    });
}

[[nodiscard]] const sorx::md::BookView& only_view(const EngineUpdate& update) {
    assert(update.publications.size() == 1);
    assert(update.publications.front().valid());
    return update.publications.front();
}

void configuration_is_strict() {
    auto invalid = FeedEngineConfig{
        VenueId{},
        ChannelId{1},
        ProfileId{1},
        1,
    };
    bool rejected = false;
    try {
        (void)FeedEngine{invalid};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    invalid.venue = VenueId{1};
    invalid.stitch_buffer_packet_capacity = 0;
    rejected = false;
    try {
        (void)FeedEngine{invalid};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        (void)engine(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void buffered_live_is_bound_to_snapshot_cut_and_high_water() {
    auto feed = engine();
    assert(feed.state() == MarketDataState::Snapshotting);

    const std::vector<FeedEvent> buffered{
        ExecuteOrderEvent{1, 17, Quantity{2}, 6},
        AddOrderEvent{4, 17, InstrumentId{101}, Side::Sell, Quantity{4}, Price{106}, 7},
    };
    const auto queued = apply_packet(feed, 44, 10, buffered, 10, 11);
    assert(queued.error == FeedError::None);
    assert(queued.state == MarketDataState::Snapshotting);
    assert(queued.publications.empty());
    assert(feed.buffered_packet_count() == 1);
    assert(feed.active_session_key() == 44);

    const auto snapshot_events = baseline_snapshot_events();
    const auto snapshot = apply_snapshot(
        feed,
        snapshot_events,
        44,
        10,
        13,
        100,
        101);
    assert(snapshot.state == MarketDataState::Stitching);
    assert(snapshot.error == FeedError::None);
    assert(snapshot.applied_message_count == snapshot_events.size() + 2);
    assert(snapshot.skipped_message_count == 0);
    assert(feed.next_sequence() == 12);
    assert(feed.stitch_cut_sequence() == 10);
    assert(feed.stitch_high_water_sequence() == 13);
    assert(feed.buffered_packet_count() == 0);

    const auto& initial = only_view(snapshot);
    assert(initial.bid_count == 1);
    assert(initial.bids[0].quantity == Quantity{10});
    assert(initial.bids[0].order_count == 2);
    assert(initial.ask_count == 2);
    assert(initial.asks[1].price == Price{106});
    assert(initial.feed_state == MarketDataState::Stitching);

    const auto premature = feed.complete_stitch(102);
    assert(premature.error == FeedError::InvalidState);
    assert(premature.publications.empty());
    assert(feed.state() == MarketDataState::Stitching);

    const std::vector<FeedEvent> through_high_water{KnownBookNeutralEvent{8}};
    const auto stitched = apply_packet(feed, 44, 12, through_high_water, 110, 111);
    assert(stitched.error == FeedError::None);
    assert(stitched.state == MarketDataState::Stitching);
    assert(feed.next_sequence() == 13);

    const auto live = feed.complete_stitch(112);
    assert(live.healthy());
    assert(live.state_changed);
    assert(only_view(live).feed_state == MarketDataState::Live);

    const std::vector<FeedEvent> overlap{
        DeleteOrderEvent{999, 17, 9},
        EndOfSessionEvent{9},
        ExecuteOrderEvent{3, 17, Quantity{4}, 10},
    };
    const auto overlapped = apply_packet(feed, 44, 11, overlap, 120, 121);
    assert(overlapped.healthy());
    assert(overlapped.skipped_message_count == 2);
    assert(overlapped.applied_message_count == 1);
    assert(feed.next_sequence() == 14);
    assert(only_view(overlapped).asks[0].quantity == Quantity{5});
}

void gap_recovery_retains_future_live_for_a_fresh_snapshot() {
    auto feed = engine();
    const auto snapshot_events = baseline_snapshot_events();
    (void)apply_snapshot(feed, snapshot_events, 77, 10, 10, 100, 101);
    assert(feed.complete_stitch(102).healthy());

    const std::vector<FeedEvent> one{KnownBookNeutralEvent{6}};
    assert(apply_packet(feed, 77, 10, one, 110, 111).healthy());
    assert(feed.next_sequence() == 11);

    const std::vector<FeedEvent> future{KnownBookNeutralEvent{7}};
    const auto gap = apply_packet(feed, 77, 12, future, 120, 121);
    assert(gap.state == MarketDataState::Recovering);
    assert(gap.error == FeedError::SequenceGap);
    assert(gap.recovery == RecoveryAction::RequestFreshSnapshot);
    assert(feed.next_sequence() == 11);
    assert(feed.buffered_packet_count() == 1);
    assert(only_view(gap).feed_state == MarketDataState::Recovering);

    const auto fresh = apply_snapshot(
        feed,
        snapshot_events,
        77,
        12,
        13,
        130,
        131);
    assert(fresh.error == FeedError::None);
    assert(fresh.state == MarketDataState::Stitching);
    assert(fresh.skipped_message_count == 0);
    assert(feed.next_sequence() == 13);
    assert(feed.complete_stitch(132).healthy());

    const auto new_session = feed.apply_packet(PacketFrame{
        78,
        1,
        0,
        {},
        140,
        141,
    });
    assert(new_session.error == FeedError::NewSession);
    assert(new_session.state == MarketDataState::Recovering);
    assert(feed.feed_session_epoch() == 2);
    assert(feed.active_session_key() == 78);
    assert(feed.buffered_packet_count() == 1);

    const auto rebound = apply_snapshot(
        feed,
        snapshot_events,
        78,
        1,
        1,
        150,
        151);
    assert(rebound.error == FeedError::None);
    assert(feed.next_sequence() == 1);
    assert(feed.complete_stitch(152).healthy());
}

void session_mismatch_and_buffer_capacity_fail_closed() {
    auto mismatched = engine();
    const std::vector<FeedEvent> neutral{KnownBookNeutralEvent{1}};
    (void)apply_packet(mismatched, 1, 5, neutral, 10, 11);
    const auto snapshot_events = baseline_snapshot_events();
    const auto rejected = apply_snapshot(
        mismatched,
        snapshot_events,
        2,
        5,
        5,
        20,
        21);
    assert(rejected.error == FeedError::NewSession);
    assert(rejected.state == MarketDataState::Recovering);
    assert(mismatched.feed_session_epoch() == 2);
    assert(mismatched.buffered_packet_count() == 0);

    const auto accepted = apply_snapshot(
        mismatched,
        snapshot_events,
        2,
        5,
        5,
        30,
        31);
    assert(accepted.error == FeedError::None);
    assert(mismatched.complete_stitch(32).healthy());

    auto bounded = engine(32, 1, 1);
    (void)apply_packet(bounded, 9, 5, neutral, 10, 11);
    const auto overflow = apply_packet(bounded, 9, 6, neutral, 12, 13);
    assert(overflow.error == FeedError::BufferCapacityExceeded);
    assert(overflow.state == MarketDataState::Recovering);
    assert(overflow.recovery == RecoveryAction::RequestFreshSnapshot);
    assert(bounded.buffered_packet_count() == 1);

    const auto recovered = apply_snapshot(
        bounded,
        snapshot_events,
        9,
        6,
        7,
        20,
        21);
    assert(recovered.error == FeedError::None);
    assert(bounded.next_sequence() == 7);
    assert(bounded.complete_stitch(22).healthy());
}

void malformed_end_and_transactional_invalid_event_are_explicit() {
    auto malformed_feed = engine();
    const std::vector<FeedEvent> one{KnownBookNeutralEvent{1}};
    const auto malformed = malformed_feed.apply_packet(PacketFrame{
        91,
        10,
        2,
        one,
        10,
        11,
    });
    assert(malformed.state == MarketDataState::Recovering);
    assert(malformed.error == FeedError::MalformedFrame);
    assert(malformed.recovery == RecoveryAction::RequestFreshSnapshot);

    auto invalid_identity = engine();
    const auto snapshot_events = baseline_snapshot_events();
    const auto invalid_snapshot = apply_snapshot(
        invalid_identity,
        snapshot_events,
        0,
        10,
        10,
        100,
        101);
    assert(invalid_snapshot.error == FeedError::InvalidSnapshot);
    assert(invalid_snapshot.state == MarketDataState::Recovering);

    auto transactional = engine();
    (void)apply_snapshot(transactional, snapshot_events, 55, 20, 20, 100, 101);
    (void)transactional.complete_stitch(102);
    const std::vector<FeedEvent> invalid_frame{
        ExecuteOrderEvent{1, 17, Quantity{2}, 6},
        DeleteOrderEvent{999, 17, 7},
    };
    const auto invalid = apply_packet(
        transactional,
        55,
        20,
        invalid_frame,
        110,
        111);
    assert(invalid.error == FeedError::InvalidEvent);
    assert(invalid.state == MarketDataState::Recovering);
    assert(invalid.applied_message_count == 1);
    assert(transactional.next_sequence() == 20);
    assert(only_view(invalid).bids[0].quantity == Quantity{12});

    auto ending = engine();
    (void)apply_snapshot(ending, snapshot_events, 92, 10, 10, 100, 101);
    (void)ending.complete_stitch(102);
    const auto end = ending.apply_packet(PacketFrame{
        92,
        10,
        std::numeric_limits<std::uint16_t>::max(),
        {},
        110,
        111,
    });
    assert(end.state == MarketDataState::Failed);
    assert(end.error == FeedError::EndOfSession);
    assert(end.end_of_session);
    assert(end.recovery == RecoveryAction::Stop);
    assert(only_view(end).feed_state == MarketDataState::Failed);
}

void clocks_and_depth_are_published_exactly() {
    auto depth_feed = engine(1);
    const std::vector<FeedEvent> deep_snapshot{
        InstrumentDirectoryEvent{17, InstrumentId{101}, 1},
        TradingStateEvent{17, InstrumentId{101}, InstrumentState::Trading, 2},
        AddOrderEvent{1, 17, InstrumentId{101}, Side::Buy, Quantity{2}, Price{100}, 3},
        AddOrderEvent{2, 17, InstrumentId{101}, Side::Buy, Quantity{3}, Price{99}, 4},
        AddOrderEvent{3, 17, InstrumentId{101}, Side::Sell, Quantity{4}, Price{105}, 5},
        AddOrderEvent{4, 17, InstrumentId{101}, Side::Sell, Quantity{5}, Price{106}, 6},
    };
    const auto snapshot = apply_snapshot(
        depth_feed,
        deep_snapshot,
        56,
        20,
        20,
        100,
        101);
    const auto& depth = only_view(snapshot);
    assert(depth.bid_count == 1);
    assert(depth.ask_count == 1);
    assert(depth.depth_truncated);
    assert(depth.bids[0].price == Price{100});
    assert(depth.asks[0].price == Price{105});
    (void)depth_feed.complete_stitch(102);

    const std::vector<FeedEvent> neutral{KnownBookNeutralEvent{7}};
    const auto regression = apply_packet(depth_feed, 56, 20, neutral, 99, 103);
    assert(regression.state == MarketDataState::Failed);
    assert(regression.error == FeedError::ClockRegression);
    const auto& failed = only_view(regression);
    assert(failed.local_receive_time_ns == 100);
    assert(failed.local_publication_time_ns == 103);
}

}  // namespace

int main() {
    configuration_is_strict();
    buffered_live_is_bound_to_snapshot_cut_and_high_water();
    gap_recovery_retains_future_live_for_a_fresh_snapshot();
    session_mismatch_and_buffer_capacity_fail_closed();
    malformed_end_and_transactional_invalid_event_are_explicit();
    clocks_and_depth_are_published_exactly();
    return 0;
}
