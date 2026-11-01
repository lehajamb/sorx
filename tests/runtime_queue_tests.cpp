#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/runtime_queues.hpp"

#include <future>
#include <string>

int main() {
    using sorx::runtime::BoundedPriorityIngressQueue;
    using sorx::runtime::BoundedQueue;
    using sorx::runtime::CapacityGate;
    using sorx::runtime::EgressClass;
    using sorx::runtime::PriorityEgressQueue;

    PriorityEgressQueue<std::string> egress(2U, 1U);

    auto normal = egress.try_reserve_normal();
    assert(normal.has_value());
    assert(!egress.normal_has_capacity());
    assert(!egress.try_reserve_normal().has_value());

    assert(egress.try_push_cancel("cancel-1"));
    assert(egress.try_push_cancel("cancel-2"));
    assert(!egress.try_push_cancel("cancel-overflow"));

    assert(egress.commit(std::move(*normal), "new-1"));
    assert(egress.pending_normal() == 1U);

    auto first = egress.try_pop();
    assert(first.has_value());
    assert(first->first == EgressClass::cancel_or_risk_off);
    assert(first->second == "cancel-1");

    auto second = egress.try_pop();
    assert(second.has_value());
    assert(second->first == EgressClass::cancel_or_risk_off);
    assert(second->second == "cancel-2");

    auto third = egress.try_pop();
    assert(third.has_value());
    assert(third->first == EgressClass::new_or_replacement);
    assert(third->second == "new-1");
    assert(!egress.try_pop().has_value());

    {
        auto released = egress.try_reserve_normal();
        assert(released.has_value());
    }
    assert(egress.normal_has_capacity());

    BoundedQueue<int> bounded(1U);
    assert(bounded.try_push(1));
    assert(!bounded.try_push(2));
    auto waiter = std::async(std::launch::async, [&bounded] {
        return bounded.wait_push(2);
    });
    assert(bounded.wait_pop() == 1);
    assert(waiter.get());
    assert(bounded.wait_pop() == 2);
    bounded.close();
    assert(!bounded.wait_push(3));
    assert(!bounded.wait_pop().has_value());

    BoundedPriorityIngressQueue<std::string> ingress(1U, 1U);
    assert(ingress.try_push_normal("normal"));
    assert(!ingress.try_push_normal("normal-overflow"));
    assert(ingress.wait_push_critical("critical"));
    assert(ingress.pending_normal() == 1U);
    assert(ingress.pending_critical() == 1U);
    assert(ingress.wait_pop() == "critical");
    assert(ingress.wait_pop() == "normal");
    ingress.close();
    assert(!ingress.wait_push_normal("closed"));
    assert(!ingress.wait_push_critical("closed"));
    assert(!ingress.wait_pop().has_value());

    CapacityGate gate(1U);
    assert(gate.try_acquire());
    assert(gate.used() == 1U);
    assert(!gate.try_acquire());
    auto gate_waiter = std::async(std::launch::async, [&gate] {
        return gate.wait_acquire();
    });
    gate.release();
    assert(gate_waiter.get());
    assert(gate.used() == 1U);
    gate.release();
    gate.close();
    assert(!gate.wait_acquire());

    return 0;
}
