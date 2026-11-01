#include "sorx/adapters/mockx_fix44.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const std::string& field(
    const sorx::adapters::mockx::FixMessage& message,
    const int tag) {
    for (const auto& item : message.fields) {
        if (item.tag == tag) {
            return item.value;
        }
    }
    throw std::runtime_error("missing field");
}

}  // namespace

int main() {
    using namespace sorx;
    using namespace sorx::adapters;
    using namespace sorx::adapters::mockx;

    MockXFix44Profile profile(MockXFix44Config{2U, 0U, 32U, "SX"});
    const std::string wire_id =
        profile.make_wire_order_id(ChildOrderId{9U}, 2U, 4U, false);
    assert(wire_id == "SXN4-2-9");

    NewOrderIntent order;
    order.venue = VenueId{1U};
    order.parent = ParentOrderId{7U};
    order.child = ChildOrderId{9U};
    order.instrument = InstrumentId{3U};
    order.side = Side::Buy;
    order.quantity = Quantity{25U};
    order.limit_price = Price{123456U};
    order.time_in_force = TimeInForce::ImmediateOrCancel;
    order.parent_revision = 2U;
    order.session_epoch = 4U;
    order.wire_order_id = wire_id;
    order.wire_symbol = "MOCK";
    const auto built = profile.make_new_order(order);
    assert(built.ok);
    assert(built.message.message_type == "D");
    assert(field(built.message, 38) == "25");
    assert(field(built.message, 44) == "1234.56");
    assert(field(built.message, 59) == "3");

    CancelOrderIntent cancel;
    cancel.venue = order.venue;
    cancel.parent = order.parent;
    cancel.child = order.child;
    cancel.side = order.side;
    cancel.original_quantity = order.quantity;
    cancel.session_epoch = order.session_epoch;
    cancel.original_wire_order_id = order.wire_order_id;
    cancel.cancel_wire_order_id =
        profile.make_wire_order_id(order.child, 2U, 4U, true);
    cancel.wire_symbol = order.wire_symbol;
    const auto canceled = profile.make_cancel(cancel);
    assert(canceled.ok);
    assert(canceled.message.message_type == "F");
    assert(field(canceled.message, 41) == wire_id);

    const std::vector<FixField> trade{
        {11, wire_id},
        {17, "EXEC-1"},
        {150, "F"},
        {39, "1"},
        {14, "5"},
        {151, "20"},
        {32, "5"},
        {31, "1234.56"},
    };
    const auto normalized =
        profile.normalize_execution_report(VenueId{1U}, 4U, trade);
    assert(normalized.ok);
    assert(normalized.report.kind == ExecutionKind::Trade);
    assert(normalized.report.last_quantity == Quantity{5U});
    assert(normalized.report.last_price == Price{123456U});
    assert(normalized.report.cumulative_quantity == Quantity{5U});

    const std::vector<FixField> imprecise{
        {11, wire_id},
        {17, "EXEC-2"},
        {150, "F"},
        {39, "1"},
        {14, "5"},
        {151, "20"},
        {32, "5"},
        {31, "1234.567"},
    };
    assert(!profile.normalize_execution_report(
                       VenueId{1U}, 4U, imprecise)
                .ok);

    const std::vector<FixField> reject{
        {11, cancel.cancel_wire_order_id},
        {41, wire_id},
        {58, "too late"},
    };
    const auto normalized_reject =
        profile.normalize_cancel_reject(VenueId{1U}, 4U, 88U, reject);
    assert(normalized_reject.ok);
    assert(normalized_reject.report.execution_id.empty());
    assert(normalized_reject.report.durable_report_key == "CXLREJ-4-88");

    return 0;
}


