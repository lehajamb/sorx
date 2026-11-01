#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/telemetry/snapshot.hpp"

#include <cassert>

int main() {
    using namespace sorx::telemetry;

    LatencyHistogram histogram;
    histogram.observe(1U);
    histogram.observe(2U);
    histogram.observe(3U);
    histogram.observe(1ULL << 40U);
    assert(histogram.samples == 4U);
    assert(histogram.maximum_ns == (1ULL << 40U));
    assert(histogram.buckets[0] == 1U);
    assert(histogram.buckets[1] == 2U);
    assert(histogram.buckets.back() == 1U);

    SnapshotPublisher publisher;
    Snapshot output;
    assert(!publisher.try_read(output));

    Snapshot invalid;
    assert(!publisher.try_publish(invalid));

    Snapshot first;
    first.generation = 41U;
    first.monotonic_time_ns = 1'000U;
    first.readiness = ReadinessState::Ready;
    first.routing_enabled = true;
    first.kill_switch_active = false;
    first.venue_count = 1U;
    first.venues[0].venue_id = 7U;
    first.venues[0].routable = true;
    first.stage_histograms[static_cast<std::size_t>(Stage::EndToEnd)] =
        histogram;
    assert(publisher.try_publish(first));
    assert(publisher.try_read(output));
    assert(output.generation == 41U);
    assert(output.venues[0].venue_id == 7U);
    assert(output.stage_histograms[7U].samples == 4U);

    Snapshot second = first;
    second.generation = 42U;
    second.monotonic_time_ns = 2'000U;
    second.venues[0].feed_gaps = 3U;
    assert(publisher.try_publish(second));
    assert(publisher.try_read(output));
    assert(output.generation == 42U);
    assert(output.venues[0].feed_gaps == 3U);

    return 0;
}
