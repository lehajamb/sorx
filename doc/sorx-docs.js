(function () {
  "use strict";

  var REDUCED_MOTION = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  function byId(id) {
    return document.getElementById(id);
  }

  function text(id, value) {
    var node = byId(id);
    if (node) node.textContent = String(value);
  }

  // Example instruments use a 0.01 price tick; fixture prices stay in integer ticks.
  function formatPrice(ticks) {
    return (Number(ticks) / 100).toFixed(2);
  }

  function clampPercent(value) {
    return Math.max(0, Math.min(100, value));
  }

  function setProgress(id, value, maximum, valueText) {
    var node = byId(id);
    node.style.width = clampPercent((value / maximum) * 100) + "%";
    node.setAttribute("aria-valuemin", "0");
    node.setAttribute("aria-valuemax", String(maximum));
    node.setAttribute("aria-valuenow", String(value));
    node.setAttribute("aria-valuetext", valueText);
  }

  function playbackStatus(frameIndex, frameCount, status) {
    if (frameIndex < 0) return "Starting · " + status;
    return "Step " + (frameIndex + 1) + " of " + frameCount + " · " + status;
  }

  function mergedState(scenario, frameIndex) {
    var state = Object.assign({}, scenario.initial);
    for (var i = 0; i <= frameIndex; i += 1) {
      Object.assign(state, scenario.frames[i]);
    }
    return state;
  }

  function setPressed(buttons, selected) {
    buttons.forEach(function (button) {
      var active = button === selected;
      button.classList.toggle("active", active);
      button.setAttribute("aria-pressed", String(active));
    });
  }

  function renderLog(container, frames, frameIndex, emptyText) {
    container.replaceChildren();
    if (frameIndex < 0) {
      var empty = document.createElement("li");
      empty.className = "empty";
      empty.textContent = emptyText;
      container.appendChild(empty);
      return;
    }

    frames.slice(0, frameIndex + 1).forEach(function (frame) {
      var row = document.createElement("li");
      row.dataset.step = frame.step;
      row.textContent = frame.log;
      container.appendChild(row);
    });
    container.scrollTop = container.scrollHeight;
  }

  function wait(ms) {
    var delay = REDUCED_MOTION ? 20 : ms;
    return new Promise(function (resolve) {
      window.setTimeout(resolve, delay);
    });
  }

  function createFramePlayer(options) {
    var scenarioKey = options.initialKey;
    var frameIndex = -1;
    var generation = 0;
    var running = false;
    var scenarioButtons = Array.from(document.querySelectorAll(options.scenarioSelector));
    var runButton = byId(options.runId);
    var stepButton = byId(options.stepId);
    var resetButton = byId(options.resetId);

    function scenario() {
      return options.scenarios[scenarioKey];
    }

    function updateControls() {
      var atEnd = frameIndex >= scenario().frames.length - 1;
      runButton.disabled = running;
      runButton.setAttribute("aria-busy", String(running));
      stepButton.disabled = running || atEnd;
      runButton.textContent = atEnd && !running ? options.replayLabel : options.runLabel;
    }

    function render() {
      options.render(scenario(), frameIndex, running);
      updateControls();
    }

    function reset(announce) {
      generation += 1;
      running = false;
      frameIndex = -1;
      render();
      if (announce && options.onReset) options.onReset(scenario());
    }

    function advance() {
      if (frameIndex >= scenario().frames.length - 1) return false;
      frameIndex += 1;
      render();
      return true;
    }

    async function run() {
      if (running) return;
      if (frameIndex >= scenario().frames.length - 1) reset(false);
      running = true;
      var myGeneration = generation + 1;
      generation = myGeneration;
      render();

      while (myGeneration === generation && frameIndex < scenario().frames.length - 1) {
        advance();
        if (frameIndex < scenario().frames.length - 1) {
          await wait(options.delay());
        }
      }

      if (myGeneration === generation) {
        running = false;
        render();
      }
    }

    scenarioButtons.forEach(function (button) {
      button.addEventListener("click", function () {
        var next = button.getAttribute(options.scenarioAttribute);
        if (!options.scenarios[next]) return;
        scenarioKey = next;
        setPressed(scenarioButtons, button);
        reset(false);
      });
    });

    runButton.addEventListener("click", run);
    stepButton.addEventListener("click", function () {
      if (!running) advance();
    });
    resetButton.addEventListener("click", function () {
      reset(true);
    });

    render();

    return {
      reset: reset,
      getScenario: function () { return scenarioKey; },
      getFrameIndex: function () { return frameIndex; },
    };
  }

  /* -----------------------------------------------------------------------
     Architecture plane explorer
     ----------------------------------------------------------------------- */

  var PLANES = {
    management: {
      kicker: "Management plane",
      heading: "Set the venues and limits a routing decision must obey",
      description:
        "Configuration defines eligible routes, priorities, conversions, and limits. RouteRib and FibCompiler turn these rules into a versioned CandidateTable. New quotes update market inputs without changing those rules.",
      path: ["INI", "ValidatedConfiguration", "RouteRib", "CandidateTable"],
      owns: [
        "route-critical identity and exact conversions",
        "route administration and configured guardrails",
        "hard risk generations and configured caps",
      ],
      notOwns: [
        "live quotes or feed freshness",
        "FIX connectivity or egress pressure",
        "parent and child lifecycle state",
      ],
      sources: [
        ["config.hpp", "../include/sorx/config.hpp"],
        ["route_rib.hpp", "../include/sorx/route_rib.hpp"],
      ],
    },
    control: {
      kicker: "Control plane",
      heading: "Keep each venue's prices and availability up to date",
      description:
        "Each venue runtime supplies its order book and whether trading is safe: feed continuity, order-entry connection, and queue capacity. The router receives a versioned BookView and readiness view for each venue.",
      path: ["UDP / GLIMPSE", "FeedEngine", "BookView generation", "CandidateRuntimeView"],
      owns: [
        "feed continuity, freshness, and trading state",
        "FIX readiness, session epoch, and egress state",
        "per-venue callback and dispatch queues",
      ],
      notOwns: [
        "route inventory or FIB compilation",
        "cross-venue atomic snapshots",
        "parent fills or risk reservation accounting",
      ],
      sources: [
        ["feed_engine.hpp", "../include/sorx/md/feed_engine.hpp"],
        ["mockx_venue_runtime.hpp", "../include/sorx/adapters/mockx_venue_runtime.hpp"],
      ],
    },
    forwarding: {
      kicker: "Forwarding plane",
      heading: "Choose child orders and track what actually happens",
      description:
        "SorCore::apply combines configured rules with current venue views, plans child orders, and accounts for reports. SorRuntime records each decision durably before releasing orders or reports to the outside world.",
      path: ["CoreEvent", "SorCore::apply", "durable journal batch", "CoreEffect"],
      owns: [
        "parents, children, fills, cancels, and replacements",
        "risk reservations, dedupe keys, and wire identity",
        "mutable kill-switch state and ordered operator events",
        "monotonic ingress and dispatch attempt sequencing",
      ],
      notOwns: [
        "protocol-specific FIX or ITCH objects",
        "venue matching behavior",
        "automatic liquidity chasing after dispatch",
      ],
      sources: [
        ["order_core.hpp", "../include/sorx/order_core.hpp"],
        ["runtime.hpp", "../include/sorx/runtime.hpp"],
      ],
    },
  };

  function installPlaneExplorer() {
    var tabs = Array.from(document.querySelectorAll(".plane-tab"));
    var detail = byId("plane-detail");
    if (!detail || tabs.length === 0) return;

    function appendList(container, items) {
      container.replaceChildren();
      items.forEach(function (item) {
        var li = document.createElement("li");
        li.textContent = item;
        container.appendChild(li);
      });
    }

    function appendPath(items) {
      var container = byId("plane-path");
      container.replaceChildren();
      items.forEach(function (item, index) {
        var node = document.createElement("span");
        node.textContent = item;
        container.appendChild(node);
        if (index < items.length - 1) {
          var arrow = document.createElement("i");
          arrow.setAttribute("aria-hidden", "true");
          arrow.textContent = "→";
          container.appendChild(arrow);
        }
      });
    }

    function appendSources(items) {
      var container = byId("plane-source");
      container.replaceChildren("Source: ");
      items.forEach(function (item, index) {
        var link = document.createElement("a");
        link.href = item[1];
        link.textContent = item[0];
        container.appendChild(link);
        if (index < items.length - 1) container.append(" · ");
      });
    }

    function selectPlane(name, moveFocus) {
      var plane = PLANES[name];
      if (!plane) return;

      tabs.forEach(function (tab) {
        var active = tab.dataset.plane === name;
        tab.classList.toggle("active", active);
        tab.setAttribute("aria-selected", String(active));
        tab.tabIndex = active ? 0 : -1;
        if (active && moveFocus) tab.focus();
      });

      detail.className = "plane-detail " + name;
      detail.setAttribute("aria-labelledby", "plane-tab-" + name);
      text("plane-kicker", plane.kicker);
      text("plane-heading", plane.heading);
      text("plane-description", plane.description);
      appendPath(plane.path);
      appendList(byId("plane-owns"), plane.owns);
      appendList(byId("plane-not-owns"), plane.notOwns);
      appendSources(plane.sources);
    }

    tabs.forEach(function (tab, index) {
      tab.addEventListener("click", function () {
        selectPlane(tab.dataset.plane, false);
      });
      tab.addEventListener("keydown", function (event) {
        var keys = ["ArrowDown", "ArrowRight", "ArrowUp", "ArrowLeft", "Home", "End"];
        if (keys.indexOf(event.key) === -1) return;
        event.preventDefault();
        var next = index;
        if (event.key === "ArrowDown" || event.key === "ArrowRight") next = (index + 1) % tabs.length;
        if (event.key === "ArrowUp" || event.key === "ArrowLeft") next = (index - 1 + tabs.length) % tabs.length;
        if (event.key === "Home") next = 0;
        if (event.key === "End") next = tabs.length - 1;
        selectPlane(tabs[next].dataset.plane, true);
      });
    });
  }

  /* -----------------------------------------------------------------------
     Routing scenarios
     ----------------------------------------------------------------------- */

  function gate(label, kind) {
    return { label: label, kind: kind };
  }

  var PENDING = gate("not evaluated", "pending");
  var ELIGIBLE = gate("eligible", "eligible");
  var SHARED_PINS = "FIB g12 · risk g1 · A book g41/adj g7 · B book g52/adj g9";

  var SHARED_VENUES = {
    A: {
      bid: "0.99 × 20",
      ask: "1.00 × 4",
      askQuantity: 4,
      allocationCap: 100,
      health: "MD LIVE · FIX READY",
      pin: "book g41 · adj g7",
      priority: "venue 1 · aggressive p1 · passive p2",
    },
    B: {
      bid: "0.98 × 20",
      ask: "1.01 × 6",
      askQuantity: 6,
      allocationCap: 100,
      health: "MD LIVE · FIX READY",
      pin: "book g52 · adj g9",
      priority: "venue 2 · aggressive p2 · passive p1",
    },
  };

  var UPDATED_VENUES = {
    A: SHARED_VENUES.A,
    B: Object.assign({}, SHARED_VENUES.B, {
      ask: "0.99 × 10", askQuantity: 10, pin: "book g53 · adj g9",
    }),
  };
  var UPDATED_PINS = "FIB g12 · risk g1 · A book g41/adj g7 · B book g53/adj g9";

  function routeInitial(overrides) {
    return Object.assign({
      phase: "Ready to plan child orders",
      phaseCopy: "Check which venues are usable, compare their prices and size, then apply limits and risk checks.",
      coreClass: "",
      aGate: PENDING,
      bGate: PENDING,
      aDepth: 0,
      bDepth: 0,
      allocated: 0,
      remainder: 0,
      children: [],
      reason: "—",
      risk: 0,
      pins: "not captured",
      exclusions: "none",
      summaryClass: "pending",
      summaryTitle: "Pending",
      summaryDetail: "No child order planned yet; no fills reported.",
      activeVenue: "",
      insight: "This replay shows an allocation plan, not a fill guarantee. Prices and displayed size can change before a child order reaches a venue; hidden liquidity is not assumed.",
      status: "Ready",
    }, overrides || {});
  }

  function child(id, venue, quantity, price, wire, tif) {
    return { id: id, venue: venue, quantity: quantity, price: price, wire: wire, tif: tif };
  }

  var ROUTE_SCENARIOS = {
    sweep: {
      label: "Split across two venues",
      order: { parent: "#1 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "101", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · buy 10 with a 1.01 limit" }),
      frames: [
        { step: "PIN", log: "Read the configured rules and the current prices, size, and availability of A and B.", phase: "Capture inputs for this decision", phaseCopy: "Each venue view has its own version. These are the inputs used for this decision, not a simultaneous market-wide snapshot.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · rules and current venue views captured" },
        { step: "GATE", log: "Both venues are enabled, their quotes are usable, their order connections are ready, and the required checks pass.", phase: "Check usable venues", phaseCopy: "Both venues can receive an immediate-or-cancel child within the parent limit.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · A and B" },
        { step: "A 1.00", log: "A has the lowest ask: 1.00 for 4 units. Allocate 4 to A; no execution has been confirmed.", phase: "Split across two venues", phaseCopy: "The best available price comes first. A offers only 4 of the requested 10 units.", activeVenue: "A", aDepth: 4, allocated: 4, remainder: 6, status: "Allocated 4 · 6 remain" },
        { step: "B 1.01", log: "B offers the next 6 units at 1.01, within the buy limit. Allocate the remaining 6 to B.", phase: "Allocate the remaining quantity", phaseCopy: "Price decides first. Configured priority and venue ID break ties at the same price.", activeVenue: "B", bDepth: 6, allocated: 10, remainder: 0, status: "Allocated 10 · fully planned" },
        { step: "PREP", log: "Prepare child orders for A: 4 at 1.00 and B: 6 at 1.01. Reserve risk for all 10 allocated units.", phase: "Prepare two child orders", phaseCopy: "Each child gets a stable order ID before it is recorded. Its limit is the worst price included in that venue's allocation.", activeVenue: "", children: [child(1, "A", 4, 100, "SXN1-1-1", "IOC"), child(2, "B", 6, 101, "SXN1-1-2", "IOC")], risk: 10, reason: "NONE", summaryClass: "partial", summaryTitle: "PREPARED", summaryDetail: "Child orders are planned; sending waits for the durable record.", status: "Prepared · two children · risk 10 held" },
        { step: "COMMIT", log: "Record the allocation, risk reservations, and child orders durably. Only then may the orders be sent.", phase: "Record the plan before sending", phaseCopy: "Accepted means SORX has recorded the plan. Venue acknowledgements and fills still have to arrive.", coreClass: "complete", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "A: 4 + B: 6 · 10 allocated · fills pending", status: "Accepted · 10 allocated · remainder 0" },
      ],
      expected: { allocated: 10, remainder: 0, reason: "NONE", risk: 10, childCount: 2 },
    },
    "new-prices": {
      label: "Same rules, new prices",
      order: { parent: "#7 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "101", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: UPDATED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · same rules, B now offers 10 at 0.99", insight: "The configured routes, priorities, caps, and parent limit are unchanged. Only B's quote changed. A new routing decision now allocates all 10 to B; an existing order would not move automatically." }),
      frames: [
        { step: "PIN", log: "Keep the same configured rules and buy limit of 1.01. B now offers 10 units at 0.99; A still offers 4 at 1.00.", phase: "Same rules, updated market data", phaseCopy: "The route configuration is still FIB g12. Only B's book advances from g52 to g53.", pins: UPDATED_PINS, coreClass: "working", status: "Pinned · unchanged rules · updated B quote" },
        { step: "GATE", log: "Both venues remain usable. The two-venue limit permits either one or two venues; it does not require a split.", phase: "Check usable venues", phaseCopy: "Both venues pass. Compare their current prices and available size next.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · A and B" },
        { step: "B 0.99", log: "B has both the better price and all 10 requested units. Allocate 10 to B.", phase: "Let the current quote decide", phaseCopy: "B wins on price even though A has the better configured aggressive priority. Priority breaks equal-price ties.", activeVenue: "B", bDepth: 10, allocated: 10, remainder: 0, status: "Allocated 10 · B only" },
        { step: "COMMIT", log: "Record one B child for 10 at 0.99 and reserve 10 units of risk before sending. No fill is confirmed by this plan.", phase: "Record the changed allocation", phaseCopy: "Same rules, different quotes: the plan changes from A 4 + B 6 to B 10.", activeVenue: "", children: [child(1, "B", 10, 99, "SXN1-1-1", "IOC")], risk: 10, reason: "NONE", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "B: 10 · same rules · fills pending", coreClass: "complete", status: "Accepted · 10 allocated to B · remainder 0" },
      ],
      expected: { allocated: 10, remainder: 0, reason: "NONE", risk: 10, childCount: 1 },
    },
    "quantity-cap": {
      label: "Cap B at 6 units",
      order: { parent: "#8 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "101", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: {
        A: UPDATED_VENUES.A,
        B: Object.assign({}, UPDATED_VENUES.B, { allocationCap: 6 }),
      },
      initial: routeInitial({ remainder: 10, status: "Ready · B offers 10, but its allocation cap is 6", insight: "A venue quantity cap limits how much this decision may allocate to that venue. B displays 10 units, but its configured cap allows only 6; the rest can go to A within the parent limit." }),
      frames: [
        { step: "PIN", log: "Use the same quotes as the previous example: B 10 at 0.99 and A 4 at 1.00. Change only B's configured allocation cap to 6.", phase: "Change one configured rule", phaseCopy: "FIB g13 carries the new B cap. Two venues are still allowed, and the buy limit remains 1.01.", pins: UPDATED_PINS.replace("FIB g12", "FIB g13"), coreClass: "working", status: "Pinned · B cap 6 · same quotes" },
        { step: "GATE", log: "Both venues pass the required checks. B is usable, but its cap limits this allocation to 6 units.", phase: "Apply the venue quantity cap", phaseCopy: "The cap changes permitted size, not B's displayed liquidity or its price rank.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · B limited to 6" },
        { step: "B 0.99", log: "B is cheapest. Allocate 6 at 0.99 and stop at B's cap, leaving 4 displayed units unused.", phase: "Allocate up to B's cap", phaseCopy: "Six units are planned for B; four requested units still need a destination.", activeVenue: "B", bDepth: 6, allocated: 6, remainder: 4, status: "Allocated 6 to B · 4 remain" },
        { step: "A 1.00", log: "A offers the remaining 4 at 1.00, within the parent limit. Allocate those 4 to A.", phase: "Allocate the rest to A", phaseCopy: "The two-venue limit permits this second child. B 6 + A 4 covers the parent quantity.", activeVenue: "A", aDepth: 4, allocated: 10, remainder: 0, status: "Allocated 10 · B 6 + A 4" },
        { step: "COMMIT", log: "Record child orders B 6 at 0.99 and A 4 at 1.00. Reserve 10 units of risk before sending.", phase: "Record the capped allocation", phaseCopy: "Current prices choose the order of allocation; configured caps constrain the quantity at each venue.", activeVenue: "", children: [child(1, "B", 6, 99, "SXN1-1-1", "IOC"), child(2, "A", 4, 100, "SXN1-1-2", "IOC")], risk: 10, reason: "NONE", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "B: 6 + A: 4 · B cap respected · fills pending", coreClass: "complete", status: "Accepted · B 6 + A 4 · remainder 0" },
      ],
      expected: { allocated: 10, remainder: 0, reason: "NONE", risk: 10, childCount: 2 },
    },
    limit: {
      label: "Tighter price limit",
      order: { parent: "#2 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "100", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · buy 10 with a 1.00 limit" }),
      frames: [
        { step: "PIN", log: "Keep the same venue quotes, but lower the parent buy limit to 1.00.", phase: "Capture inputs for this decision", phaseCopy: "A buy limit of 1.00 permits prices at or below 1.00.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · limit 1.00" },
        { step: "GATE", log: "Both venues are usable. Next, compare their asks with the parent limit.", phase: "Check usable venues", phaseCopy: "A venue can be healthy while its price is too expensive for this order.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · evaluate price next" },
        { step: "A 1.00", log: "Allocate A's 4 units at 1.00. B's 1.01 ask is above the 1.00 buy limit.", phase: "Stop at the price limit", phaseCopy: "Six units remain unallocated. The four allocated units still need a venue execution report before they count as filled.", activeVenue: "A", aDepth: 4, allocated: 4, remainder: 6, status: "Allocated 4 · limit blocks 6" },
        { step: "PREP", log: "Prepare one A child for 4 units and reserve 4 units of risk.", phase: "Prepare partial route", phaseCopy: "The decision reports 4 allocated and 6 unallocated. It does not automatically retry the remainder when quotes change.", activeVenue: "", children: [child(1, "A", 4, 100, "SXN1-1-1", "IOC")], risk: 4, reason: "PARENT LIMIT REACHED", summaryClass: "partial", summaryTitle: "PARTIAL ROUTE", summaryDetail: "A: 4 · 6 unallocated · fills pending", status: "Prepared · 4 allocated · remainder 6" },
        { step: "COMMIT", log: "Record the A child and the reason for the 6 unallocated units: parent price limit reached.", phase: "Record the plan before sending", phaseCopy: "A later quote change does not by itself create another child order for the remainder.", coreClass: "complete", status: "Accepted · remainder PARENT LIMIT REACHED" },
      ],
      expected: { allocated: 4, remainder: 6, reason: "PARENT LIMIT REACHED", risk: 4, childCount: 1 },
    },
    "venue-cap": {
      label: "Venue-count cap",
      order: { parent: "#3 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "101", maxVenues: "1", ranking: "price → priority → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · max venues 1" }),
      frames: [
        { step: "PIN", log: "Use the same quotes, but allow this decision to send orders to only one venue.", phase: "Capture inputs for this decision", phaseCopy: "This limit counts venues. It is separate from the quantity allowed at each venue.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · venue cap 1" },
        { step: "GATE", log: "Both A and B are usable before applying the one-venue limit.", phase: "Check usable venues", phaseCopy: "The best price still determines which venue is chosen first.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · A and B" },
        { step: "A 1.00", log: "Select A at 1.00 and allocate its 4 displayed units.", phase: "Select first venue", phaseCopy: "The one allowed venue slot now belongs to A.", activeVenue: "A", aDepth: 4, allocated: 4, remainder: 6, status: "Allocated 4 · one allowed venue selected" },
        { step: "CAP", log: "B would be a second venue, so the configured venue-count limit prevents a B child.", phase: "Block a new venue", phaseCopy: "More eligible depth at A could still be allocated. This example has only 4 units at A.", activeVenue: "", bGate: gate("maximum venue count", "excluded"), exclusions: "B · MAXIMUM VENUE COUNT", status: "B excluded · 6 remain" },
        { step: "COMMIT", log: "Record one A child for 4 units and report the remaining 6 as unallocated.", phase: "Commit partial decision", phaseCopy: "4 allocated + 6 unallocated = 10 requested. Allocation is still separate from execution.", children: [child(1, "A", 4, 100, "SXN1-1-1", "IOC")], risk: 4, reason: "MAXIMUM VENUE COUNT", summaryClass: "partial", summaryTitle: "PARTIAL ROUTE", summaryDetail: "A: 4 · 6 unallocated because only one venue is allowed", coreClass: "complete", status: "Accepted · remainder MAXIMUM VENUE COUNT" },
      ],
      expected: { allocated: 4, remainder: 6, reason: "MAXIMUM VENUE COUNT", risk: 4, childCount: 1 },
    },
    passive: {
      label: "Rest on one venue",
      order: { parent: "#4 · ALPHA", intent: "Passive GTC", sideQuantity: "BUY 10", quantity: 10, limit: "99", maxVenues: "exactly 1", ranking: "passive priority → configured latency → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · rest a buy for 10 at 0.99", insight: "A passive GTC order rests on one venue. SORX first checks that it would not immediately trade on any otherwise eligible venue, then selects one venue by passive priority." }),
      frames: [
        { step: "PIN", log: "Read both books and the configured passive venue priorities.", phase: "Capture inputs for this decision", phaseCopy: "Both asks are above the 0.99 buy limit, so the order would rest.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · passive buy at 0.99" },
        { step: "GATE", log: "Neither book offers a seller at or below 0.99. The order is passive on both venues.", phase: "Check the order would rest", phaseCopy: "If this buy could trade immediately on any otherwise eligible route, SORX would reject the passive request.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Non-marketable · candidates eligible" },
        { step: "RANK", log: "B has the better configured passive priority, so it receives the full 10-unit child.", phase: "Choose one resting venue", phaseCopy: "Passive routing chooses one venue; it does not split the quantity across queues.", activeVenue: "B", allocated: 10, remainder: 0, status: "Selected B · passive priority 1" },
        { step: "COMMIT", log: "Record a B child to buy 10 at 0.99 and reserve 10 units of risk before sending.", phase: "Commit single-venue child", phaseCopy: "GTC means good till canceled. A changed quote does not automatically move this resting order to another venue.", activeVenue: "", children: [child(1, "B", 10, 99, "SXN1-1-1", "GTC")], risk: 10, reason: "NONE", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "B: 10 at 0.99 · resting order planned · fills pending", coreClass: "complete", status: "Accepted · one B child" },
      ],
      expected: { allocated: 10, remainder: 0, reason: "NONE", risk: 10, childCount: 1 },
    },
    marketable: {
      label: "Reject a crossing passive order",
      order: { parent: "#5 · ALPHA", intent: "Passive GTC", sideQuantity: "BUY 10", quantity: 10, limit: "100", maxVenues: "exactly 1", ranking: "passive priority → configured latency → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · passive buy 10 at 1.00", insight: "Passive means the order should rest. A buy at 1.00 would match A's 1.00 ask immediately, so this passive request is rejected as a whole." }),
      frames: [
        { step: "PIN", log: "Read the books: A's best ask is 1.00, equal to the requested passive buy limit.", phase: "Capture inputs for this decision", phaseCopy: "Check all otherwise eligible venues before choosing a resting venue.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · passive buy at 1.00" },
        { step: "GATE", log: "Both venues have usable market data and ready order connections.", phase: "Check usable venues", phaseCopy: "B's passive priority cannot override a buy that would immediately trade at A.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · test marketability" },
        { step: "CROSS", log: "A offers stock at 1.00, which the 1.00 buy can take. This request is not passive.", phase: "Reject the crossing passive request", phaseCopy: "To request an immediate trade, the caller can instead submit an aggressive IOC with a price limit.", activeVenue: "A", aGate: gate("triggers reject", "excluded"), status: "Rejected · A makes parent marketable" },
        { step: "REJECT", log: "Record the rejection. No child is created, no risk is reserved, and all 10 units remain unallocated.", phase: "Report why no child was created", phaseCopy: "The reason is a passive order that would trade immediately. The technical remainder code is PassiveFullQuantityUnavailable.", activeVenue: "", allocated: 0, remainder: 10, reason: "PASSIVE FULL QUANTITY UNAVAILABLE", risk: 0, exclusions: "none · parent-level rejection", summaryClass: "rejected", summaryTitle: "PASSIVE MARKETABLE", summaryDetail: "0 children · 10 unallocated", coreClass: "rejected", status: "Rejected · PASSIVE MARKETABLE" },
      ],
      expected: { allocated: 0, remainder: 10, reason: "PASSIVE FULL QUANTITY UNAVAILABLE", risk: 0, childCount: 0 },
    },
    withdrawn: {
      label: "Skip a venue with missing data",
      order: { parent: "#6 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 3", quantity: 3, limit: "101", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: {
        A: Object.assign({}, SHARED_VENUES.A, { health: "MD RECOVERING · FIX READY" }),
        B: SHARED_VENUES.B,
      },
      initial: routeInitial({ remainder: 3, status: "Ready · A recovering", insight: "An attractive quote is not enough: its feed must be trustworthy. A is excluded during recovery; B can still receive a new order." }),
      frames: [
        { step: "PIN", log: "A's feed is recovering; B still has a current, continuous book.", phase: "Capture inputs for this decision", phaseCopy: "A's displayed price cannot be used for new routing while its market data is recovering.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · A RECOVERING · B LIVE" },
        { step: "GATE", log: "Exclude A because its feed is not LIVE. B passes the required checks.", phase: "Exclude the unsafe quote", phaseCopy: "The decision records why A was skipped and which book versions it used.", aGate: gate("MD not LIVE", "excluded"), bGate: ELIGIBLE, exclusions: "A · MARKET DATA NOT LIVE", status: "A excluded · B eligible" },
        { step: "B 1.01", log: "Allocate all 3 requested units to B at 1.01, within the parent limit.", phase: "Allocate to the available venue", phaseCopy: "No quantity is allocated to A while its feed recovers.", activeVenue: "B", bDepth: 3, allocated: 3, remainder: 0, status: "Allocated 3 · venue B" },
        { step: "COMMIT", log: "Record a B child for 3 units and reserve 3 units of risk before sending.", phase: "Commit healthy route", phaseCopy: "A can be considered for later decisions only after its feed returns to LIVE and the other checks pass.", activeVenue: "", children: [child(1, "B", 3, 101, "SXN1-1-1", "IOC")], risk: 3, reason: "NONE", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "B: 3 · A excluded · fills pending", coreClass: "complete", status: "Accepted · B unaffected" },
      ],
      expected: { allocated: 3, remainder: 0, reason: "NONE", risk: 3, childCount: 1 },
    },
  };

  function renderGate(id, value) {
    var node = byId(id);
    node.textContent = value.label;
    node.className = "status-badge " + value.kind;
  }

  function renderChildren(container, children) {
    container.replaceChildren();
    if (children.length === 0) {
      var empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "No child order has been prepared.";
      container.appendChild(empty);
      return;
    }
    children.forEach(function (item) {
      var card = document.createElement("article");
      card.className = "child-card";
      var label = document.createElement("span");
      var terms = document.createElement("strong");
      var wire = document.createElement("small");
      label.textContent = "Child #" + item.id + " · venue " + item.venue;
      terms.textContent = item.quantity + " @ " + formatPrice(item.price) + " · " + item.tif;
      wire.textContent = item.wire;
      card.append(label, terms, wire);
      container.appendChild(card);
    });
  }

  function renderRoute(scenario, frameIndex, running) {
    var state = mergedState(scenario, frameIndex);
    var venueA = scenario.venues.A;
    var venueB = scenario.venues.B;

    text("route-parent", scenario.order.parent);
    text("route-intent", scenario.order.intent);
    text("route-order", scenario.order.sideQuantity);
    text("route-limit", formatPrice(scenario.order.limit));
    text("route-max-venues", scenario.order.maxVenues);
    text("route-ranking", scenario.order.ranking);

    text("route-a-bid", venueA.bid);
    text("route-a-ask", venueA.ask);
    text("route-a-health", venueA.health);
    text("route-a-pin", venueA.pin);
    text("route-a-priority", venueA.priority);
    text("route-a-cap", venueA.allocationCap + " units per decision");
    text("route-b-bid", venueB.bid);
    text("route-b-ask", venueB.ask);
    text("route-b-health", venueB.health);
    text("route-b-pin", venueB.pin);
    text("route-b-priority", venueB.priority);
    text("route-b-cap", venueB.allocationCap + " units per decision");
    renderGate("route-a-gate", state.aGate);
    renderGate("route-b-gate", state.bGate);

    var cardA = byId("route-venue-a");
    var cardB = byId("route-venue-b");
    cardA.classList.toggle("active", state.activeVenue === "A");
    cardB.classList.toggle("active", state.activeVenue === "B");
    cardA.classList.toggle("excluded", state.aGate.kind === "excluded");
    cardB.classList.toggle("excluded", state.bGate.kind === "excluded");
    setProgress("route-a-depth", state.aDepth, venueA.askQuantity, state.aDepth + " of " + venueA.askQuantity + " displayed ask units allocated; fills not confirmed");
    setProgress("route-b-depth", state.bDepth, venueB.askQuantity, state.bDepth + " of " + venueB.askQuantity + " displayed ask units allocated; fills not confirmed");

    text("route-phase", state.phase);
    text("route-phase-copy", state.phaseCopy);
    byId("route-decision-core").className = "decision-core " + state.coreClass;
    text("route-allocated", state.allocated);
    text("route-remainder", state.remainder);
    setProgress("route-meter-fill", state.allocated, scenario.order.quantity, state.allocated + " of " + scenario.order.quantity + " units allocated");
    renderLog(byId("route-log"), scenario.frames, frameIndex, "Run or step to see how rules and quotes produce a child-order plan.");

    var summary = byId("route-summary");
    summary.className = "decision-summary " + state.summaryClass;
    summary.children[1].textContent = state.summaryTitle;
    summary.children[2].textContent = state.summaryDetail;
    renderChildren(byId("route-children"), state.children);
    text("route-remainder-reason", state.reason);
    text("route-risk", state.risk);
    text("route-pins", state.pins);
    text("route-exclusions", state.exclusions);

    var insight = byId("route-insight").querySelector("p");
    insight.replaceChildren();
    var strong = document.createElement("strong");
    strong.textContent = "Remember. ";
    insight.append(strong, state.insight);
    text("route-status", running ? playbackStatus(frameIndex, scenario.frames.length, state.status) : state.status);
  }

  function validateRouteFixtures() {
    Object.keys(ROUTE_SCENARIOS).forEach(function (key) {
      var scenario = ROUTE_SCENARIOS[key];
      var finalState = mergedState(scenario, scenario.frames.length - 1);
      var childTotal = finalState.children.reduce(function (sum, item) { return sum + item.quantity; }, 0);
      if (finalState.allocated + finalState.remainder !== scenario.order.quantity) {
        throw new Error("Route fixture does not conserve parent quantity: " + key);
      }
      if (childTotal !== finalState.allocated) {
        throw new Error("Route fixture children do not match allocation: " + key);
      }
      ["allocated", "remainder", "reason", "risk"].forEach(function (field) {
        if (finalState[field] !== scenario.expected[field]) {
          throw new Error("Route fixture expected " + field + " mismatch: " + key);
        }
      });
      if (finalState.children.length !== scenario.expected.childCount) {
        throw new Error("Route fixture child count mismatch: " + key);
      }
      finalState.children.forEach(function (item) {
        if (item.quantity > scenario.venues[item.venue].allocationCap) {
          throw new Error("Route fixture exceeds venue quantity cap: " + key);
        }
      });
    });
    return true;
  }

  function installRouteLab() {
    var speed = byId("route-speed");
    var output = byId("route-speed-output");
    speed.addEventListener("input", function () {
      output.value = speed.value + "×";
      output.textContent = speed.value + "×";
    });

    createFramePlayer({
      scenarios: ROUTE_SCENARIOS,
      initialKey: "sweep",
      scenarioSelector: "[data-route-scenario]",
      scenarioAttribute: "data-route-scenario",
      runId: "route-run",
      stepId: "route-step",
      resetId: "route-reset",
      runLabel: "Run decision",
      replayLabel: "Replay decision",
      delay: function () { return 1100 / Number(speed.value); },
      render: renderRoute,
      onReset: function (scenario) { text("route-status", "Reset · " + scenario.label); },
    });
  }

  /* -----------------------------------------------------------------------
     Lifecycle, journal, and crash points
     ----------------------------------------------------------------------- */

  function lifeInitial(overrides) {
    return Object.assign({
      parentId: "#1 · revision 1",
      parentState: "ROUTING",
      parentCopy: "waiting for a recorded child-order plan",
      parentClass: "",
      children: [],
      risk: 0,
      riskTotal: 10,
      riskCopy: "Risk is reserved when child orders are prepared. A venue report is still needed to confirm a fill.",
      riskClass: "normal",
      event: "ParentSubmit",
      record: "not committed",
      effect: "blocked",
      receipt: false,
      receiptText: "not issued",
      status: "Ready",
    }, overrides || {});
  }

  var LIFE_SCENARIOS = {
    fill: {
      label: "All 10 fill",
      initial: lifeInitial({ status: "Ready · parent #1" }),
      frames: [
        { step: "COMMIT", log: "Record a plan to send 4 units to A and 6 to B. Reserve risk for all 10; none is filled yet.", parentState: "ACTIVE", parentCopy: "A: 4 + B: 6 allocated · 0 filled so far", children: [{ label: "Child #1 · A · 4 @ 1.00", state: "PREPARED", copy: "wire SXN1-1-1 · risk 4", className: "" }, { label: "Child #2 · B · 6 @ 1.01", state: "PREPARED", copy: "wire SXN1-1-2 · risk 6", className: "" }], risk: 10, event: "ParentSubmit", record: "RouteDecision · ChildPrepared ×2 · StateImage", effect: "child orders may now queue for sending", receipt: true, receiptText: "Accepted · ingress #1 · durable", status: "Durably accepted · venue outcome pending" },
        { step: "POLL", log: "Before sending each child, record its dispatch attempt. This lets recovery distinguish orders that may have reached a venue.", children: [{ label: "Child #1 · A · 4 @ 1.00", state: "DISPATCH STARTED", copy: "attempt 1 · SXN1-1-1", className: "" }, { label: "Child #2 · B · 6 @ 1.01", state: "DISPATCH STARTED", copy: "attempt 2 · SXN1-1-2", className: "" }], event: "poll_egress(A/B)", record: "DispatchStarted ×2", effect: "FIX New A · FIX New B", status: "Dispatch started · risk 10 held" },
        { step: "LOCAL", log: "The local connection accepts both sends. This does not yet mean the venues acknowledged or filled them, so the quantity must stay reserved.", children: [{ label: "Child #1 · A · 4 @ 1.00", state: "PENDING ACK", copy: "accepted locally · risk 4", className: "" }, { label: "Child #2 · B · 6 @ 1.01", state: "PENDING ACK", copy: "accepted locally · risk 6", className: "" }], event: "LocalDispatchEvent", record: "DispatchResult ×2", effect: "10 units still reserved · venue outcome pending", status: "Pending acknowledgement · no reroute" },
        { step: "ACK", log: "Both venues acknowledge the orders. The children are working, but acknowledgements are not fills. Duplicate reports are ignored.", children: [{ label: "Child #1 · A · 4 @ 1.00", state: "WORKING", copy: "ACK-A · risk 4", className: "" }, { label: "Child #2 · B · 6 @ 1.01", state: "WORKING", copy: "ACK-B · risk 6", className: "" }], event: "ExecutionReportEvent", record: "ExecutionReportObserved ×2 · ExecutionCanonicalized ×2", effect: "none · child states updated", status: "Working · 0 filled · risk 10 held" },
        { step: "FILL", log: "A confirms 4 filled and B confirms 6 filled. Both children are finished, their reservations are released, and the parent is filled.", parentState: "FILLED", parentCopy: "10 / 10 filled · 0 unfilled", parentClass: "terminal", children: [{ label: "Child #1 · A · 4 @ 1.00", state: "FILLED", copy: "filled 4 · risk 0", className: "terminal" }, { label: "Child #2 · B · 6 @ 1.01", state: "FILLED", copy: "filled 6 · risk 0", className: "terminal" }], risk: 0, riskCopy: "Both children have definite final outcomes, so no reservation remains.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ChildTerminal · ParentTerminal", effect: "ParentReport · FILLED", status: "Filled 10 / 10 · risk 0" },
      ],
      expected: { parentState: "FILLED", risk: 0 },
    },
    partial: {
      label: "Only 6 of 10 fill",
      initial: lifeInitial({ parentId: "#2 · revision 1", parentState: "ACTIVE", parentCopy: "A: 4 + B: 6 allocated · both working · 0 filled", parentQuantity: 10, filled: 0, unfilled: 0, children: [{ label: "Child #1 · A · 4 @ 1.00", state: "WORKING", copy: "filled 0 · remaining 4 · risk 4", quantity: 4, filled: 0, leaves: 4, risk: 4, className: "" }, { label: "Child #2 · B · 6 @ 1.01", state: "WORKING", copy: "filled 0 · remaining 6 · risk 6", quantity: 6, filled: 0, leaves: 6, risk: 6, className: "" }], risk: 10, riskCopy: "Both children are working. Their full reservations remain held in the reference router.", event: "Venue reports pending", record: "Child orders recorded earlier", effect: "await venue reports", receipt: true, receiptText: "Original submit accepted earlier", status: "Ready · 10 allocated does not guarantee 10 filled" }),
      frames: [
        { step: "A FILLS", log: "A confirms all 4 units filled. A is finished, so release its 4-unit reservation. B still has 6 working.", parentCopy: "4 filled · 6 still working at B", filled: 4, children: [{ label: "Child #1 · A · 4 @ 1.00", state: "FILLED", copy: "filled 4 · remaining 0 · risk 0", quantity: 4, filled: 4, leaves: 0, risk: 0, className: "terminal" }, { label: "Child #2 · B · 6 @ 1.01", state: "WORKING", copy: "filled 0 · remaining 6 · risk 6", quantity: 6, filled: 0, leaves: 6, risk: 6, className: "" }], risk: 6, riskCopy: "A's 4-unit reservation is released; B's 6-unit reservation is still held.", event: "ExecutionReportEvent · A filled 4", record: "ExecutionOutcome · ChildTerminal(A)", effect: "ParentReport · PARTIALLY FILLED", status: "Filled 4 / 10 · B working 6 · risk 6 held" },
        { step: "B PARTIAL", log: "B confirms only 2 of its 6 units filled, leaving 4 working. The parent has 6 confirmed fills, not 10.", parentCopy: "6 filled · 4 still working at B", filled: 6, children: [{ label: "Child #1 · A · 4 @ 1.00", state: "FILLED", copy: "filled 4 · remaining 0 · risk 0", quantity: 4, filled: 4, leaves: 0, risk: 0, className: "terminal" }, { label: "Child #2 · B · 6 @ 1.01", state: "WORKING", copy: "filled 2 · remaining 4 · risk 6 held until terminal", quantity: 6, filled: 2, leaves: 4, risk: 6, className: "" }], risk: 6, riskCopy: "The reference router conservatively holds B's whole 6-unit reservation until B finishes, even though only 4 units remain working.", event: "ExecutionReportEvent · B partially filled 2", record: "ExecutionOutcome(B) · state image", effect: "ParentReport · PARTIALLY FILLED", status: "Filled 6 / 10 · B working 4 · risk 6 held" },
        { step: "B CANCELED", log: "B confirms the IOC left 4 units unfilled and canceled them. B is now finished: 2 filled and 4 canceled. Release its full 6-unit reservation.", parentState: "CANCELED", parentCopy: "6 / 10 filled · 4 / 10 unfilled · none working", parentClass: "terminal", filled: 6, unfilled: 4, children: [{ label: "Child #1 · A · 4 @ 1.00", state: "FILLED", copy: "filled 4 · unfilled 0 · risk 0", quantity: 4, filled: 4, leaves: 0, unfilled: 0, risk: 0, className: "terminal" }, { label: "Child #2 · B · 6 @ 1.01", state: "CANCELED", copy: "filled 2 · canceled 4 · risk 0", quantity: 6, filled: 2, leaves: 0, unfilled: 4, risk: 0, className: "terminal" }], risk: 0, riskCopy: "Both venue outcomes are final. All reservations are released; the 4 unfilled units are not automatically rerouted.", event: "ExecutionReportEvent · B canceled remaining 4", record: "ExecutionOutcome · ChildTerminal(B) · ParentTerminal", effect: "ParentReport · CANCELED · filled 6", status: "Finished · 6 filled · 4 unfilled · risk 0" },
      ],
      expected: { parentState: "CANCELED", risk: 0, filled: 6, unfilled: 4 },
    },
    cancel: {
      label: "Cancel",
      initial: lifeInitial({ parentId: "#10 · revision 1", parentState: "ACTIVE", parentCopy: "5 working at B · cancel uses that existing order", children: [{ label: "Child #1 · B · 5 @ 0.99", state: "WORKING", copy: "epoch 2 · SXN2-1-1 · risk 5", className: "" }], risk: 5, riskTotal: 5, riskCopy: "The reservation stays held until a definite final venue outcome.", receipt: true, receiptText: "Original submit accepted earlier", status: "Ready · cancel parent #10" }),
      frames: [
        { step: "CANCEL", log: "Record the cancel request and send it to the existing B child using its saved session and order ID. A fresh quote is not needed to cancel.", parentState: "CANCEL PENDING", parentCopy: "cancel requested · venue confirmation pending", parentClass: "cancel-pending", children: [{ label: "Child #1 · B · 5 @ 0.99", state: "CANCEL PENDING", copy: "OrigClOrdID SXN2-1-1 · risk 5", className: "cancel-pending" }], event: "ParentCancel", record: "ParentCancel · state image", effect: "cancel queued with priority over new orders", receipt: true, receiptText: "Cancel accepted durably", status: "Cancel pending · risk 5 held" },
        { step: "PRIORITY", log: "Send the cancel ahead of queued new orders, after recording its dispatch attempt. Risk remains held while the cancel is pending.", event: "poll_egress(B)", record: "DispatchStarted · cancel attempt", effect: "FIX Cancel B / epoch 2", status: "Cancel dispatched before normal egress" },
        { step: "DONE", log: "B confirms the 5-unit child is canceled. Its final outcome is known, so the reservation is released.", parentState: "CANCELED", parentCopy: "0 filled · 5 canceled and unfilled · none working", parentClass: "terminal", children: [{ label: "Child #1 · B · 5 @ 0.99", state: "CANCELED", copy: "definite venue outcome · risk 0", className: "terminal" }], risk: 0, riskCopy: "The confirmed final outcome releases the reservation.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ChildTerminal · ParentTerminal", effect: "ParentReport · CANCELED", status: "Canceled · risk 0" },
      ],
      expected: { parentState: "CANCELED", risk: 0 },
    },
    replace: {
      label: "Replace",
      initial: lifeInitial({ parentId: "#20 · revision 1", parentState: "ACTIVE", parentCopy: "passive B child · 6 @ 0.99", children: [{ label: "Old child · B · 6 @ 0.99", state: "WORKING", copy: "SXN2-1-2 · risk 6", className: "" }], risk: 6, riskTotal: 8, riskCopy: "The existing 6-unit child still holds its reservation.", receipt: true, receiptText: "Original submit accepted earlier", status: "Ready · replace total 8 @ 0.98" }),
      frames: [
        { step: "INTENT", log: "Record the requested replacement: total 8 at 0.98. First cancel the existing 6-unit child; do not send the replacement yet.", parentState: "REPLACE PENDING", parentCopy: "waiting for the old child to finish before routing the replacement", parentClass: "cancel-pending", children: [{ label: "Old child · B · 6 @ 0.99", state: "CANCEL PENDING", copy: "SXN2-1-2 · risk 6", className: "cancel-pending" }], event: "ParentReplace", record: "ParentReplace · state image", effect: "cancel effect queued after commit", receipt: true, receiptText: "Replace intent accepted durably", status: "Replace pending · old quantity still owned" },
        { step: "POLL CANCEL", log: "Record the cancel dispatch attempt, then send the cancel to B. The old 6-unit reservation remains held.", event: "poll_egress(B)", record: "DispatchStarted · cancel attempt", effect: "FIX Cancel old child", status: "Cancel dispatched · old risk 6 held" },
        { step: "ACCOUNT + REV 2", log: "B confirms the old child is canceled. In one recorded update, release its reservation and plan the replacement: 8 at 0.98 on B.", parentId: "#20 · revision 2", parentState: "ACTIVE", parentCopy: "replacement planned · B: 8 at 0.98 · fills pending", parentClass: "", children: [{ label: "Old child · B · 6 @ 0.99", state: "CANCELED", copy: "revision 1 · risk 0", className: "terminal" }, { label: "New child · B · 8 @ 0.98", state: "PREPARED", copy: "SXN2-2-3 · risk 8", className: "" }], risk: 8, riskCopy: "The same update releases the old 6-unit reservation and reserves 8 for the new child.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ChildTerminal · RouteDecision · ChildPrepared", effect: "new effect queued after same durable batch", status: "Replacement prepared · risk 8 held" },
        { step: "POLL NEW", log: "Record the replacement's dispatch attempt, then send the new 8-unit child.", children: [{ label: "Old child · B · 6 @ 0.99", state: "CANCELED", copy: "revision 1 · risk 0", className: "terminal" }, { label: "New child · B · 8 @ 0.98", state: "DISPATCH STARTED", copy: "SXN2-2-3 · risk 8", className: "" }], event: "poll_egress(B)", record: "DispatchStarted · new attempt", effect: "FIX New revision 2", status: "Revision 2 dispatch started · risk 8 held" },
        { step: "FILL", log: "B confirms all 8 units of the replacement filled. Release its reservation and finish the parent.", parentState: "FILLED", parentCopy: "revision 2 · filled 8 / 8", parentClass: "terminal", children: [{ label: "Old child · B · 6 @ 0.99", state: "CANCELED", copy: "revision 1 · risk 0", className: "terminal" }, { label: "New child · B · 8 @ 0.98", state: "FILLED", copy: "revision 2 · risk 0", className: "terminal" }], risk: 0, riskCopy: "The replacement child is fully filled, so its reservation is released.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ParentTerminal", effect: "ParentReport · FILLED", status: "Replace complete · filled · risk 0" },
      ],
      expected: { parentState: "FILLED", risk: 0 },
    },
    unknown: {
      label: "Uncertain send",
      initial: lifeInitial({ parentId: "#20 · revision 1", parentState: "ACTIVE", parentCopy: "A may have received the 5-unit child; its outcome is not known", children: [{ label: "Child #1 · A · 5 @ 1.00", state: "DISPATCH STARTED", copy: "SXN1-1-1 · risk 5", className: "" }], risk: 5, riskTotal: 5, riskCopy: "All 5 units stay reserved while the child may exist at A.", event: "DispatchStarted", record: "DispatchStarted · attempt 1", effect: "wire outcome pending", receipt: true, receiptText: "Submit accepted durably", status: "Ready · simulate an uncertain send" }),
      frames: [
        { step: "UNKNOWN", log: "The connection cannot tell whether A received the order. SORX must treat those 5 units as potentially live.", parentState: "SUSPENDED", parentCopy: "uncertain outcome · check the existing order at A before acting", parentClass: "ambiguous", children: [{ label: "Child #1 · A · 5 @ 1.00", state: "AMBIGUOUS", copy: "wire SXN1-1-1 · risk 5 HELD", className: "ambiguous" }], risk: 5, riskCopy: "Uncertain is not canceled or unfilled. Keep all 5 units reserved until the venue outcome is reconciled.", riskClass: "danger", event: "LocalDispatchEvent · UNKNOWN", record: "DispatchResult · state image", effect: "none · no reroute", status: "Suspended · ambiguous · risk 5 held" },
        { step: "OWNED", log: "Do not send the same 5 units to B: A might still fill them. Reconciliation must establish what happened to the existing child.", event: "reconciliation required", record: "ownership preserved", effect: "new routing for these 5 units prohibited", status: "No reroute · A may still fill the order" },
      ],
      expected: { parentState: "SUSPENDED", risk: 5 },
    },
  };

  function renderLifeChildren(container, items) {
    container.replaceChildren();
    if (items.length === 0) {
      var empty = document.createElement("div");
      empty.className = "state-card child-state muted";
      var label = document.createElement("span");
      var state = document.createElement("strong");
      var copy = document.createElement("small");
      label.textContent = "Children";
      state.textContent = "NONE";
      copy.textContent = "no child order has been prepared";
      empty.append(label, state, copy);
      container.appendChild(empty);
      return;
    }

    items.forEach(function (item) {
      var card = document.createElement("div");
      card.className = "state-card child-state " + item.className;
      var label = document.createElement("span");
      var state = document.createElement("strong");
      var copy = document.createElement("small");
      label.textContent = item.label;
      state.textContent = item.state;
      copy.textContent = item.copy;
      card.append(label, state, copy);
      container.appendChild(card);
    });
  }

  function renderLife(scenario, frameIndex, running) {
    var state = mergedState(scenario, frameIndex);
    text("life-parent-id", state.parentId);
    text("life-parent-state", state.parentState);
    text("life-parent-copy", state.parentCopy);
    var parentCard = byId("life-parent-state").parentElement;
    parentCard.className = "state-card parent-state " + state.parentClass;
    renderLifeChildren(byId("life-children"), state.children);

    text("life-risk-label", state.risk + " / " + state.riskTotal + " held");
    text("life-risk-copy", state.riskCopy);
    var riskFill = byId("life-risk-fill");
    setProgress("life-risk-fill", state.risk, state.riskTotal, state.risk + " of " + state.riskTotal + " units held");
    riskFill.style.background = state.riskClass === "danger" ? "var(--danger)" : "var(--persistence)";

    text("life-event", state.event);
    text("life-record", state.record);
    text("life-effect", state.effect);
    var receipt = byId("life-receipt");
    receipt.className = "receipt-card" + (state.receipt ? " committed" : "");
    receipt.children[1].textContent = state.receiptText;
    renderLog(byId("life-log"), scenario.frames, frameIndex, "Run or step to separate a recorded plan, a sent order, and a confirmed venue outcome.");
    text("life-status", running ? playbackStatus(frameIndex, scenario.frames.length, state.status) : state.status);
  }

  function validateLifeFixtures() {
    Object.keys(LIFE_SCENARIOS).forEach(function (key) {
      var scenario = LIFE_SCENARIOS[key];
      var finalState = mergedState(scenario, scenario.frames.length - 1);
      if (finalState.parentState !== scenario.expected.parentState || finalState.risk !== scenario.expected.risk) {
        throw new Error("Lifecycle fixture mismatch: " + key);
      }
      if (finalState.parentState === "SUSPENDED" && finalState.risk === 0) {
        throw new Error("Ambiguous lifecycle released risk: " + key);
      }
    });
    return true;
  }

  var CRASH_POINTS = {
    before: {
      badge: "NO RECEIPT",
      badgeClass: "pending",
      heading: "Before recording: no order has been released",
      copy: "At this selected point the command has not been recorded and no order has been released. The caller can retry; a missing receipt alone is not proof that an arbitrary send was absent.",
    },
    prepared: {
      badge: "DEFINITELY UNSENT",
      badgeClass: "committed",
      heading: "Recorded but definitely unsent: resume the same child",
      copy: "Recovery can send this child with its already recorded order ID. It records a new dispatch attempt without inventing a second order.",
    },
    wire: {
      badge: "OWNERSHIP UNCERTAIN",
      badgeClass: "ambiguous",
      heading: "The order may already be live at the venue",
      copy: "Mark the child AMBIGUOUS, suspend the parent, and keep its risk reserved. Reconcile with the venue before reusing that quantity elsewhere.",
    },
    terminal: {
      badge: "DEFINITE OUTCOME",
      badgeClass: "eligible",
      heading: "A confirmed final outcome releases the reservation",
      copy: "A full fill, confirmed cancel, or definite rejection finishes the child. Record it once, release its reservation, and update the parent; repeated reports do not count twice.",
    },
  };

  function installCrashExplorer() {
    var buttons = Array.from(document.querySelectorAll("[data-crash-point]"));
    var detail = byId("crash-detail");
    buttons.forEach(function (button) {
      button.addEventListener("click", function () {
        var item = CRASH_POINTS[button.dataset.crashPoint];
        setPressed(buttons, button);
        detail.replaceChildren();
        var badge = document.createElement("span");
        var heading = document.createElement("h4");
        var copy = document.createElement("p");
        badge.className = "status-badge " + item.badgeClass;
        badge.textContent = item.badge;
        heading.textContent = item.heading;
        copy.textContent = item.copy;
        detail.append(badge, heading, copy);
      });
    });
  }

  function installLifeLab() {
    createFramePlayer({
      scenarios: LIFE_SCENARIOS,
      initialKey: "fill",
      scenarioSelector: "[data-life-scenario]",
      scenarioAttribute: "data-life-scenario",
      runId: "life-run",
      stepId: "life-step",
      resetId: "life-reset",
      runLabel: "Run lifecycle",
      replayLabel: "Replay lifecycle",
      delay: function () { return 620; },
      render: renderLife,
      onReset: function (scenario) { text("life-status", "Reset · " + scenario.label); },
    });
    installCrashExplorer();
  }

  /* -----------------------------------------------------------------------
     Feed continuity and restart recovery
     ----------------------------------------------------------------------- */

  function feedInitial(overrides) {
    return Object.assign({
      state: "SNAPSHOTTING",
      session: "—",
      cut: "—",
      next: "1",
      highWater: "—",
      buffered: "0",
      book: "no usable book published yet",
      action: "waiting for a complete starting book and matching live updates",
      status: "Ready",
    }, overrides || {});
  }

  var FEED_SCENARIOS = {
    stitch: {
      label: "Build a usable order book",
      initial: feedInitial(),
      frames: [
        { step: "BUFFER", log: "Save live update 10 while the starting book is being downloaded. It belongs to feed session 44, but the book is not yet safe for routing.", session: "44", next: "1", buffered: "1", status: "Live update saved · book not ready for routing" },
        { step: "CUT 10", log: "Install the starting book at sequence 10 and replay saved updates through 11. The next required sequence is 12; the known catch-up target is 13.", state: "STITCHING", cut: "10", next: "12", highWater: "13", buffered: "0", book: "bid 1.00 × 10 · asks 1.05 × 9 / 1.06 × 4", action: "Catching up · next 12 must reach target 13 before routing", status: "Snapshot installed · catching up" },
        { step: "SEQ 12", log: "Apply the expected update 12, which does not change these displayed prices. No update is missing before the catch-up target 13.", next: "13", action: "Caught up · the feed can now switch to LIVE", status: "Caught up · next 13" },
        { step: "LIVE", log: "Publish the continuous book as LIVE. Later routing decisions may use it if freshness and the other venue checks also pass.", state: "LIVE", action: "LIVE · current book is available to routing checks", status: "LIVE · no missing updates in the reconstructed book" },
      ],
      expected: { state: "LIVE", next: "13" },
    },
    gap: {
      label: "Recover from a missing update",
      initial: feedInitial({ state: "LIVE", session: "77", cut: "9", next: "10", highWater: "10", book: "bid 1.00 × 12 · ask 1.05 × 9", action: "LIVE · waiting for sequence 10", status: "Ready · LIVE session 77" }),
      frames: [
        { step: "SEQ 10", log: "Apply update 10 as expected. The book stays LIVE and the next expected update is 11.", next: "11", highWater: "10", action: "LIVE · next expected 11", status: "LIVE · next 11 · high-water 10" },
        { step: "GAP", log: "Update 12 arrives before 11. The displayed book may be wrong, so immediately mark the feed RECOVERING and request a fresh starting book.", state: "RECOVERING", cut: "—", highWater: "—", buffered: "1", book: "Old prices are for inspection only · excluded from new routing", action: "Missing update 11 · exclude this venue's quotes and rebuild its book", status: "RECOVERING · quote withdrawn from new routing" },
        { step: "CUT 12", log: "Install a fresh book starting at sequence 12 and apply the saved update 12. The reconstructed sequence now reaches target 13.", state: "STITCHING", cut: "12", next: "13", highWater: "13", buffered: "0", book: "fresh book + saved update 12 · ready to verify", action: "Caught up to target 13 · final continuity check pending", status: "Fresh book installed · market-data continuity restored" },
        { step: "LIVE", log: "Publish the rebuilt book as LIVE. New routing decisions may consider this venue again; existing orders do not move just because the feed recovers.", state: "LIVE", action: "LIVE · rebuilt book is available to routing checks", status: "LIVE again · continuous book restored" },
      ],
      expected: { state: "LIVE", next: "13" },
    },
  };

  function renderFeed(scenario, frameIndex, running) {
    var state = mergedState(scenario, frameIndex);
    Array.from(document.querySelectorAll("[data-feed-state]")).forEach(function (node) {
      var active = node.dataset.feedState === state.state;
      node.classList.toggle("active", active);
      if (active) node.setAttribute("aria-current", "step");
      else node.removeAttribute("aria-current");
    });
    text("feed-session", state.session);
    text("feed-cut", state.cut);
    text("feed-next", state.next);
    text("feed-high-water", state.highWater);
    text("feed-buffered", state.buffered);
    text("feed-book", state.book);
    text("feed-action", state.action);
    renderLog(byId("feed-log"), scenario.frames, frameIndex, "Run or step to see when displayed quotes become safe to use for routing.");
    text("feed-status", running ? playbackStatus(frameIndex, scenario.frames.length, state.status) : state.status);
  }

  function validateFeedFixtures() {
    Object.keys(FEED_SCENARIOS).forEach(function (key) {
      var scenario = FEED_SCENARIOS[key];
      var finalState = mergedState(scenario, scenario.frames.length - 1);
      if (finalState.state !== scenario.expected.state || finalState.next !== scenario.expected.next) {
        throw new Error("Feed fixture mismatch: " + key);
      }
      if (key === "gap" && !scenario.frames.some(function (frame) { return frame.state === "RECOVERING"; })) {
        throw new Error("Gap fixture never withdraws LIVE");
      }
    });
    return true;
  }

  function installFeedLab() {
    createFramePlayer({
      scenarios: FEED_SCENARIOS,
      initialKey: "stitch",
      scenarioSelector: "[data-feed-scenario]",
      scenarioAttribute: "data-feed-scenario",
      runId: "feed-run",
      stepId: "feed-step",
      resetId: "feed-reset",
      runLabel: "Run feed",
      replayLabel: "Replay feed",
      delay: function () { return 620; },
      render: renderFeed,
      onReset: function (scenario) { text("feed-status", "Reset · " + scenario.label); },
    });
  }

  function installRecoveryGate() {
    var rows = Array.from(document.querySelectorAll("#recovery-gates li"));
    var outcomeButtons = Array.from(document.querySelectorAll("[data-recovery-outcome]"));
    var runButton = byId("recovery-run");
    var stepButton = byId("recovery-step");
    var resetButton = byId("recovery-reset");
    var gate = byId("routing-gate");
    var outcome = "resolved";
    var completed = 0;
    var generation = 0;
    var running = false;
    var blocked = false;

    function render() {
      rows.forEach(function (row, index) {
        var isBlocked = blocked && index === 3;
        row.classList.toggle("done", index < completed);
        row.classList.toggle("current", !blocked && index === completed && completed < rows.length);
        row.classList.toggle("blocked", isBlocked);
        row.querySelector("b").textContent = isBlocked
          ? "blocked"
          : (index < completed ? "done" : (index === completed && completed < rows.length ? "next" : "pending"));
      });

      var open = outcome === "resolved" && !blocked && completed === rows.length;
      gate.className = "routing-gate " + (open ? "open" : "closed");
      gate.children[1].textContent = open ? "ROUTING ENABLED" : "RECOVERY ONLY";
      gate.children[2].textContent = blocked
        ? "An order may still be live at a venue. Keep its risk reserved and resolve its outcome before reopening routing."
        : (open ? "Recovery checks passed; new parent orders may be routed." : "Complete the recovery checks before accepting new routing decisions.");

      if (blocked) {
        text("recovery-status", "Blocked at child reconciliation · AMBIGUOUS risk held · routing closed");
      } else {
        text("recovery-status", open ? "5 / 5 gates satisfied · routing enabled" : completed + " / 5 gates satisfied");
      }

      runButton.disabled = running || blocked;
      runButton.setAttribute("aria-busy", String(running));
      stepButton.disabled = running || open || blocked;
      runButton.textContent = blocked ? "Recovery blocked" : (open && !running ? "Replay recovery" : "Run recovery");
    }

    function reset(announce) {
      generation += 1;
      running = false;
      completed = 0;
      blocked = false;
      render();
      if (announce) text("recovery-status", "Reset · 0 / 5 gates satisfied");
    }

    function advance() {
      if (completed === 3 && outcome === "ambiguous") {
        blocked = true;
        render();
        return false;
      }
      if (completed < rows.length) completed += 1;
      render();
      return true;
    }

    async function run() {
      if (running || blocked) return;
      if (completed === rows.length) reset(false);
      running = true;
      var myGeneration = generation + 1;
      generation = myGeneration;
      render();
      while (myGeneration === generation && completed < rows.length && !blocked) {
        if (!advance()) break;
        if (completed < rows.length && !blocked) await wait(600);
      }
      if (myGeneration === generation) {
        running = false;
        render();
      }
    }

    outcomeButtons.forEach(function (button) {
      button.addEventListener("click", function () {
        outcome = button.getAttribute("data-recovery-outcome");
        setPressed(outcomeButtons, button);
        reset(false);
        text("recovery-status", outcome === "ambiguous"
          ? "Uncertain order selected · resolve its venue outcome before routing resumes"
          : "All order outcomes known · 0 / 5 recovery checks complete");
      });
    });

    runButton.addEventListener("click", run);
    stepButton.addEventListener("click", function () { if (!running && !blocked) advance(); });
    resetButton.addEventListener("click", function () { reset(true); });
    render();
  }

  function installSectionNavigation() {
    var links = Array.from(document.querySelectorAll('.navlinks a[href^="#"]'));
    if (links.length === 0) return;

    function markCurrent(id) {
      links.forEach(function (link) {
        if (link.getAttribute("href") === "#" + id) link.setAttribute("aria-current", "location");
        else link.removeAttribute("aria-current");
      });
    }

    var initialHash = window.location && window.location.hash ? window.location.hash.slice(1) : "overview";
    if (!byId(initialHash)) initialHash = "overview";
    markCurrent(initialHash);

    if (typeof window.IntersectionObserver !== "function") return;
    var observer = new window.IntersectionObserver(function (entries) {
      var visible = entries.filter(function (entry) { return entry.isIntersecting; })
        .sort(function (a, b) { return b.intersectionRatio - a.intersectionRatio; });
      if (visible.length > 0) markCurrent(visible[0].target.id);
    }, { rootMargin: "-18% 0px -68% 0px", threshold: [0, 0.2, 0.5] });

    links.forEach(function (link) {
      var section = byId(link.getAttribute("href").slice(1));
      if (section) observer.observe(section);
    });
  }

  /* -----------------------------------------------------------------------
     Boot and self-validation
     ----------------------------------------------------------------------- */

  var validation = {
    routes: validateRouteFixtures(),
    lifecycle: validateLifeFixtures(),
    feed: validateFeedFixtures(),
  };
  document.documentElement.dataset.scenariosValid =
    validation.routes && validation.lifecycle && validation.feed ? "true" : "false";

  installPlaneExplorer();
  installRouteLab();
  installLifeLab();
  installFeedLab();
  installRecoveryGate();
  installSectionNavigation();

  window.SorxDocs = Object.freeze({
    routeScenarios: ROUTE_SCENARIOS,
    lifecycleScenarios: LIFE_SCENARIOS,
    feedScenarios: FEED_SCENARIOS,
    validation: Object.freeze(validation),
    reducedMotion: REDUCED_MOTION,
  });
})();

