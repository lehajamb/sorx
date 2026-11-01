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
      heading: "Compile route intent into an immutable generation",
      description:
        "Strict configuration becomes an authoritative RouteRib, then FibCompiler publishes a generation-stamped CandidateTable. Quotes and transient disconnects never rebuild it.",
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
      heading: "Publish immutable readiness, never borrowed market state",
      description:
        "Each venue runtime owns feed recovery, an order-entry session, bounded queues, and a versioned adjacency view. The core receives owned BookView values and normalized state only.",
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
      heading: "Serialize decisions, accounting, and side effects",
      description:
        "SorCore::apply is the deterministic state machine and sole order-state owner. SorRuntime sequences ingress, persists complete batches, then releases reports and dispatch effects.",
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
      bid: "99 × 20",
      ask: "100 × 4",
      askQuantity: 4,
      health: "MD LIVE · FIX READY",
      pin: "book g41 · adj g7",
      priority: "venue 1 · aggressive p1 · passive p2",
    },
    B: {
      bid: "98 × 20",
      ask: "101 × 6",
      askQuantity: 6,
      health: "MD LIVE · FIX READY",
      pin: "book g52 · adj g9",
      priority: "venue 2 · aggressive p2 · passive p1",
    },
  };

  function routeInitial(overrides) {
    return Object.assign({
      phase: "Ready to evaluate",
      phaseCopy: "No child exists until candidate evaluation, allocation, and aggregate risk checks finish.",
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
      summaryDetail: "no durable child intent",
      activeVenue: "",
      insight: "Displayed depth only. The policy never infers hidden liquidity, and truncated depth remains explicitly unknown.",
      status: "Ready",
    }, overrides || {});
  }

  function child(id, venue, quantity, price, wire, tif) {
    return { id: id, venue: venue, quantity: quantity, price: price, wire: wire, tif: tif };
  }

  var ROUTE_SCENARIOS = {
    sweep: {
      label: "Sweep best price",
      order: { parent: "#1 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "101", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · BUY 10 IOC @ 101" }),
      frames: [
        { step: "PIN", log: "Capture FIB g12, risk g1, and each venue's adjacency/book generation.", phase: "Pin immutable inputs", phaseCopy: "The decision records per-venue views; it does not claim one atomic market snapshot.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · per-venue generations captured" },
        { step: "GATE", log: "A and B pass admin, intent, book, freshness, trading, FIX, egress, exactness, and risk gates.", phase: "Evaluate candidates", phaseCopy: "Both routes are eligible for protected IOC allocation.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · A and B" },
        { step: "A 100", log: "Best canonical ask is A at 100. Consume all 4 displayed units.", phase: "Sweep best price", phaseCopy: "Venue A is selected first at the best canonical price.", activeVenue: "A", aDepth: 4, allocated: 4, remainder: 6, status: "Allocated 4 · 6 remain" },
        { step: "B 101", log: "Next executable ask is B at 101. Consume 6 within the parent protection.", phase: "Continue k-way sweep", phaseCopy: "Price precedes configured priority; priority and venue ID only break equal prices.", activeVenue: "B", bDepth: 6, allocated: 10, remainder: 0, status: "Allocated 10 · fully planned" },
        { step: "PREP", log: "Create one child per venue at that venue's worst retained level; reserve 10 units of risk.", phase: "Prepare children + risk", phaseCopy: "New and cancel wire IDs are generated before ChildPrepared becomes durable.", activeVenue: "", children: [child(1, "A", 4, 100, "SXN1-1-1", "IOC"), child(2, "B", 6, 101, "SXN1-1-2", "IOC")], risk: 10, reason: "NONE", summaryClass: "partial", summaryTitle: "PREPARED", summaryDetail: "effects still behind the commit boundary", status: "Prepared · two children · risk 10 held" },
        { step: "COMMIT", log: "Append RouteDecision, reservations, ChildPrepared records, and a full state image; then release effects.", phase: "Commit before wire", phaseCopy: "The successful receipt now means validation + ingress sequence + durable journal commit.", coreClass: "complete", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "2 children · 10 allocated · 0 remainder", status: "Accepted · 10 allocated · remainder 0" },
      ],
      expected: { allocated: 10, remainder: 0, reason: "NONE", risk: 10, childCount: 2 },
    },
    limit: {
      label: "Protection limit",
      order: { parent: "#2 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "100", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · BUY 10 IOC @ 100" }),
      frames: [
        { step: "PIN", log: "Capture the same per-venue generations with a tighter parent protection price.", phase: "Pin immutable inputs", phaseCopy: "The parent may execute only at canonical prices ≤ 100.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · limit 100" },
        { step: "GATE", log: "Both candidates pass common eligibility. Executability is decided by the sweep.", phase: "Evaluate candidates", phaseCopy: "B is healthy and eligible, but its ask may still be beyond the protection limit.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · evaluate price next" },
        { step: "A 100", log: "Consume A's 4 displayed units at 100. B's 101 ask is outside the parent limit.", phase: "Stop at protection", phaseCopy: "The unfilled quantity is explicit; SORX does not turn it into an unbounded market order.", activeVenue: "A", aDepth: 4, allocated: 4, remainder: 6, status: "Allocated 4 · limit blocks 6" },
        { step: "PREP", log: "Prepare one A child and reserve only its 4 allocated units.", phase: "Prepare partial route", phaseCopy: "The parent retains an explicit six-unit remainder.", activeVenue: "", children: [child(1, "A", 4, 100, "SXN1-1-1", "IOC")], risk: 4, reason: "PARENT LIMIT REACHED", summaryClass: "partial", summaryTitle: "PARTIAL ROUTE", summaryDetail: "1 child · 4 allocated · 6 explicit remainder", status: "Prepared · 4 allocated · remainder 6" },
        { step: "COMMIT", log: "Commit the partial decision and its explicit ParentLimitReached remainder before dispatch.", phase: "Commit before wire", phaseCopy: "The remainder is a deterministic outcome, not a request to chase more liquidity.", coreClass: "complete", status: "Accepted · remainder PARENT LIMIT REACHED" },
      ],
      expected: { allocated: 4, remainder: 6, reason: "PARENT LIMIT REACHED", risk: 4, childCount: 1 },
    },
    "venue-cap": {
      label: "Venue-count cap",
      order: { parent: "#3 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 10", quantity: 10, limit: "101", maxVenues: "1", ranking: "price → priority → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · max venues 1" }),
      frames: [
        { step: "PIN", log: "Capture both venue views and maximum_aggressive_venues = 1.", phase: "Pin immutable inputs", phaseCopy: "The venue cap constrains new venue selection, not depth already selected.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · venue cap 1" },
        { step: "GATE", log: "A and B pass common eligibility.", phase: "Evaluate candidates", phaseCopy: "Both are healthy before the venue-count rule is applied.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · A and B" },
        { step: "A 100", log: "Select A at 100 and consume all 4 displayed units.", phase: "Select first venue", phaseCopy: "The one allowed venue slot now belongs to A.", activeVenue: "A", aDepth: 4, allocated: 4, remainder: 6, status: "Allocated 4 · venue slot consumed" },
        { step: "CAP", log: "B would introduce a second venue, so exclude it with MaximumVenueCount.", phase: "Block a new venue", phaseCopy: "If A had more executable levels, they could still be consumed after the cap.", activeVenue: "", bGate: gate("maximum venue count", "excluded"), exclusions: "B · MAXIMUM VENUE COUNT", status: "B excluded · 6 remain" },
        { step: "COMMIT", log: "Prepare A's child and commit the explicit six-unit remainder.", phase: "Commit partial decision", phaseCopy: "Quantity is conserved: 4 allocated + 6 remainder = 10 requested.", children: [child(1, "A", 4, 100, "SXN1-1-1", "IOC")], risk: 4, reason: "MAXIMUM VENUE COUNT", summaryClass: "partial", summaryTitle: "PARTIAL ROUTE", summaryDetail: "venue cap prevents a B child", coreClass: "complete", status: "Accepted · remainder MAXIMUM VENUE COUNT" },
      ],
      expected: { allocated: 4, remainder: 6, reason: "MAXIMUM VENUE COUNT", risk: 4, childCount: 1 },
    },
    passive: {
      label: "Passive choice",
      order: { parent: "#4 · ALPHA", intent: "Passive GTC", sideQuantity: "BUY 10", quantity: 10, limit: "99", maxVenues: "exactly 1", ranking: "passive priority → latency → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · BUY 10 GTC @ 99", insight: "Passive GTC must be non-marketable everywhere and routes the full allowed quantity to exactly one venue." }),
      frames: [
        { step: "PIN", log: "Capture both healthy books and the passive ranking metadata.", phase: "Pin immutable inputs", phaseCopy: "A and B expose asks above the 99 passive limit.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · passive @ 99" },
        { step: "GATE", log: "Both venues pass common eligibility; the order is non-marketable on both books.", phase: "Prove non-marketable", phaseCopy: "A passive parent is rejected if it crosses any otherwise healthy route.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Non-marketable · candidates eligible" },
        { step: "RANK", log: "B wins on passive priority 1 before configured latency or venue ID are considered.", phase: "Rank one venue", phaseCopy: "The full quantity remains on one deterministic venue; there is no passive splitting.", activeVenue: "B", allocated: 10, remainder: 0, status: "Selected B · passive priority 1" },
        { step: "COMMIT", log: "Prepare one B GTC child at 99, reserve 10 units, and commit before dispatch.", phase: "Commit single-venue child", phaseCopy: "The child stays until fill, cancel, or cancel-then-reroute replace.", activeVenue: "", children: [child(1, "B", 10, 99, "SXN1-1-1", "GTC")], risk: 10, reason: "NONE", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "1 passive child · venue B · 10 @ 99", coreClass: "complete", status: "Accepted · one B child" },
      ],
      expected: { allocated: 10, remainder: 0, reason: "NONE", risk: 10, childCount: 1 },
    },
    marketable: {
      label: "Passive marketable",
      order: { parent: "#5 · ALPHA", intent: "Passive GTC", sideQuantity: "BUY 10", quantity: 10, limit: "100", maxVenues: "exactly 1", ranking: "passive priority → latency → venue ID" },
      venues: SHARED_VENUES,
      initial: routeInitial({ remainder: 10, status: "Ready · BUY 10 GTC @ 100", insight: "A passive parent is rejected as a whole if it is marketable on any otherwise healthy route." }),
      frames: [
        { step: "PIN", log: "Capture both books. A's best ask equals the parent's 100 limit.", phase: "Pin immutable inputs", phaseCopy: "The marketability check uses every common-eligible route.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · passive @ 100" },
        { step: "GATE", log: "A and B pass common readiness checks.", phase: "Evaluate common gates", phaseCopy: "Passive capability ranking happens only after the global marketability check.", aGate: ELIGIBLE, bGate: ELIGIBLE, status: "Eligible · test marketability" },
        { step: "CROSS", log: "A ask 100 ≤ BUY limit 100, so the PassiveGTC parent is marketable.", phase: "Reject marketable passive", phaseCopy: "The caller must use a protected IOC for the crossing quantity.", activeVenue: "A", aGate: gate("triggers reject", "excluded"), status: "Rejected · A makes parent marketable" },
        { step: "REJECT", log: "Commit no child and reserve no risk. The full quantity remains explicit.", phase: "Return deterministic rejection", phaseCopy: "No venue owns any part of the parent.", activeVenue: "", allocated: 0, remainder: 10, reason: "PASSIVE FULL QUANTITY UNAVAILABLE", risk: 0, exclusions: "none · parent-level rejection", summaryClass: "rejected", summaryTitle: "PASSIVE MARKETABLE", summaryDetail: "0 children · 10 unallocated", coreClass: "rejected", status: "Rejected · PASSIVE MARKETABLE" },
      ],
      expected: { allocated: 0, remainder: 10, reason: "PASSIVE FULL QUANTITY UNAVAILABLE", risk: 0, childCount: 0 },
    },
    withdrawn: {
      label: "Venue withdrawn",
      order: { parent: "#6 · ALPHA", intent: "Aggressive IOC", sideQuantity: "BUY 3", quantity: 3, limit: "101", maxVenues: "2", ranking: "price → priority → venue ID" },
      venues: {
        A: Object.assign({}, SHARED_VENUES.A, { health: "MD RECOVERING · FIX READY" }),
        B: SHARED_VENUES.B,
      },
      initial: routeInitial({ remainder: 3, status: "Ready · A recovering", insight: "Feed failure withdraws A from new price-sensitive routing. B remains independent and available." }),
      frames: [
        { step: "PIN", log: "Capture A's non-LIVE generation and B's healthy LIVE view.", phase: "Pin immutable inputs", phaseCopy: "Health is per venue; one failed feed does not stop the other venue.", pins: SHARED_PINS, coreClass: "working", status: "Pinned · A RECOVERING · B LIVE" },
        { step: "GATE", log: "Exclude A because market data is not LIVE. B passes every gate.", phase: "Withdraw unhealthy venue", phaseCopy: "The exclusion is explicit and versioned in the decision.", aGate: gate("MD not LIVE", "excluded"), bGate: ELIGIBLE, exclusions: "A · MARKET DATA NOT LIVE", status: "A excluded · B eligible" },
        { step: "B 101", log: "Consume 3 displayed units from B at the 101 protection limit.", phase: "Route around withdrawal", phaseCopy: "No A quantity is assumed or chased while its feed recovers.", activeVenue: "B", bDepth: 3, allocated: 3, remainder: 0, status: "Allocated 3 · venue B" },
        { step: "COMMIT", log: "Prepare one B child, reserve 3 units, and commit before dispatch.", phase: "Commit healthy route", phaseCopy: "A remains excluded until it publishes a fresh LIVE generation.", activeVenue: "", children: [child(1, "B", 3, 101, "SXN1-1-1", "IOC")], risk: 3, reason: "NONE", summaryClass: "accepted", summaryTitle: "ACCEPTED DURABLY", summaryDetail: "1 B child · A explicitly excluded", coreClass: "complete", status: "Accepted · B unaffected" },
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
      empty.textContent = "No child owns quantity in this decision.";
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
      terms.textContent = item.quantity + " @ " + item.price + " · " + item.tif;
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
    text("route-limit", scenario.order.limit);
    text("route-max-venues", scenario.order.maxVenues);
    text("route-ranking", scenario.order.ranking);

    text("route-a-bid", venueA.bid);
    text("route-a-ask", venueA.ask);
    text("route-a-health", venueA.health);
    text("route-a-pin", venueA.pin);
    text("route-a-priority", venueA.priority);
    text("route-b-bid", venueB.bid);
    text("route-b-ask", venueB.ask);
    text("route-b-health", venueB.health);
    text("route-b-pin", venueB.pin);
    text("route-b-priority", venueB.priority);
    renderGate("route-a-gate", state.aGate);
    renderGate("route-b-gate", state.bGate);

    var cardA = byId("route-venue-a");
    var cardB = byId("route-venue-b");
    cardA.classList.toggle("active", state.activeVenue === "A");
    cardB.classList.toggle("active", state.activeVenue === "B");
    cardA.classList.toggle("excluded", state.aGate.kind === "excluded");
    cardB.classList.toggle("excluded", state.bGate.kind === "excluded");
    setProgress("route-a-depth", state.aDepth, venueA.askQuantity, state.aDepth + " of " + venueA.askQuantity + " displayed ask units consumed");
    setProgress("route-b-depth", state.bDepth, venueB.askQuantity, state.bDepth + " of " + venueB.askQuantity + " displayed ask units consumed");

    text("route-phase", state.phase);
    text("route-phase-copy", state.phaseCopy);
    byId("route-decision-core").className = "decision-core " + state.coreClass;
    text("route-allocated", state.allocated);
    text("route-remainder", state.remainder);
    setProgress("route-meter-fill", state.allocated, scenario.order.quantity, state.allocated + " of " + scenario.order.quantity + " units allocated");
    renderLog(byId("route-log"), scenario.frames, frameIndex, "Run or step to inspect the deterministic decision.");

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
      parentCopy: "waiting for a durable route decision",
      parentClass: "",
      children: [],
      risk: 0,
      riskTotal: 10,
      riskCopy: "Reservations are created with ChildPrepared.",
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
      label: "Happy fill",
      initial: lifeInitial({ status: "Ready · parent #1" }),
      frames: [
        { step: "COMMIT", log: "Validate parent, plan 4 + 6, reserve risk, and commit ChildPrepared ×2 with a full state image.", parentState: "ACTIVE", parentCopy: "two durable Prepared children own all 10 units", children: [{ label: "Child #1 · A · 4 @ 100", state: "PREPARED", copy: "wire SXN1-1-1 · risk 4", className: "" }, { label: "Child #2 · B · 6 @ 101", state: "PREPARED", copy: "wire SXN1-1-2 · risk 6", className: "" }], risk: 10, event: "ParentSubmit", record: "RouteDecision · ChildPrepared ×2 · StateImage", effect: "effects may now queue", receipt: true, receiptText: "Accepted · ingress #1 · durable", status: "Durably accepted · venue outcome pending" },
        { step: "POLL", log: "Poll egress: commit DispatchStarted before returning each NewOrderSingle effect.", children: [{ label: "Child #1 · A · 4 @ 100", state: "DISPATCH STARTED", copy: "attempt 1 · SXN1-1-1", className: "" }, { label: "Child #2 · B · 6 @ 101", state: "DISPATCH STARTED", copy: "attempt 2 · SXN1-1-2", className: "" }], event: "poll_egress(A/B)", record: "DispatchStarted ×2", effect: "FIX New A · FIX New B", status: "Dispatch started · risk 10 held" },
        { step: "LOCAL", log: "Both FIX submissions are accepted locally; quantity can no longer be proved absent.", children: [{ label: "Child #1 · A · 4 @ 100", state: "PENDING ACK", copy: "accepted locally · risk 4", className: "" }, { label: "Child #2 · B · 6 @ 101", state: "PENDING ACK", copy: "accepted locally · risk 6", className: "" }], event: "LocalDispatchEvent", record: "DispatchResult ×2", effect: "quantity remains owned", status: "Pending acknowledgement · no reroute" },
        { step: "ACK", log: "Normalized execution reports move both children to Working; reports are deduped by venue/session/ExecID.", children: [{ label: "Child #1 · A · 4 @ 100", state: "WORKING", copy: "ACK-A · risk 4", className: "" }, { label: "Child #2 · B · 6 @ 101", state: "WORKING", copy: "ACK-B · risk 6", className: "" }], event: "ExecutionReportEvent", record: "ExecutionReportObserved · ExecutionCanonicalized ×2", effect: "none · child states updated", status: "Working · 0 filled · risk 10 held" },
        { step: "FILL", log: "Definite fills account 4 + 6, release both reservations, and make parent and children terminal.", parentState: "FILLED", parentCopy: "10 / 10 filled · quantity conserved", parentClass: "terminal", children: [{ label: "Child #1 · A · 4 @ 100", state: "FILLED", copy: "filled 4 · risk 0", className: "terminal" }, { label: "Child #2 · B · 6 @ 101", state: "FILLED", copy: "filled 6 · risk 0", className: "terminal" }], risk: 0, riskCopy: "Terminal accounting released every reserved unit.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ChildTerminal · ParentTerminal", effect: "ParentReport · FILLED", status: "Filled 10 / 10 · risk 0" },
      ],
      expected: { parentState: "FILLED", risk: 0 },
    },
    cancel: {
      label: "Cancel",
      initial: lifeInitial({ parentId: "#10 · revision 1", parentState: "ACTIVE", parentCopy: "one working B child · feed health irrelevant to cancel", children: [{ label: "Child #1 · B · 5 @ 99", state: "WORKING", copy: "epoch 2 · SXN2-1-1 · risk 5", className: "" }], risk: 5, riskTotal: 5, riskCopy: "Working quantity remains reserved.", receipt: true, receiptText: "Original submit accepted earlier", status: "Ready · cancel parent #10" }),
      frames: [
        { step: "CANCEL", log: "Commit ParentCancel and target the child's recorded B / epoch 2 mapping—no fresh route lookup.", parentState: "CANCEL PENDING", parentCopy: "waiting for the known child to become terminal", parentClass: "cancel-pending", children: [{ label: "Child #1 · B · 5 @ 99", state: "CANCEL PENDING", copy: "OrigClOrdID SXN2-1-1 · risk 5", className: "cancel-pending" }], event: "ParentCancel", record: "ParentCancel · state image", effect: "reserved cancel lane", receipt: true, receiptText: "Cancel accepted durably", status: "Cancel pending · risk 5 held" },
        { step: "PRIORITY", log: "poll_egress drains cancel/risk-off before queued new traffic and commits its DispatchStarted attempt.", event: "poll_egress(B)", record: "DispatchStarted · cancel attempt", effect: "FIX Cancel B / epoch 2", status: "Cancel dispatched before normal egress" },
        { step: "DONE", log: "A definite canceled report makes parent and child terminal and releases the five-unit reservation.", parentState: "CANCELED", parentCopy: "terminal · no working quantity", parentClass: "terminal", children: [{ label: "Child #1 · B · 5 @ 99", state: "CANCELED", copy: "definite venue outcome · risk 0", className: "terminal" }], risk: 0, riskCopy: "Definite terminal accounting releases risk.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ChildTerminal · ParentTerminal", effect: "ParentReport · CANCELED", status: "Canceled · risk 0" },
      ],
      expected: { parentState: "CANCELED", risk: 0 },
    },
    replace: {
      label: "Replace",
      initial: lifeInitial({ parentId: "#20 · revision 1", parentState: "ACTIVE", parentCopy: "passive B child · 6 @ 99", children: [{ label: "Old child · B · 6 @ 99", state: "WORKING", copy: "SXN2-1-2 · risk 6", className: "" }], risk: 6, riskTotal: 8, riskCopy: "Old revision owns six units.", receipt: true, receiptText: "Original submit accepted earlier", status: "Ready · replace total 8 @ 98" }),
      frames: [
        { step: "INTENT", log: "Commit replace intent with new total 8 and limit 98; do not route the new revision yet.", parentState: "REPLACE PENDING", parentCopy: "cancel old revision before rerouting", parentClass: "cancel-pending", children: [{ label: "Old child · B · 6 @ 99", state: "CANCEL PENDING", copy: "SXN2-1-2 · risk 6", className: "cancel-pending" }], event: "ParentReplace", record: "ParentReplace · state image", effect: "cancel effect queued after commit", receipt: true, receiptText: "Replace intent accepted durably", status: "Replace pending · old quantity still owned" },
        { step: "POLL CANCEL", log: "A later poll commits the cancel DispatchStarted attempt before returning the cancel effect.", event: "poll_egress(B)", record: "DispatchStarted · cancel attempt", effect: "FIX Cancel old child", status: "Cancel dispatched · old risk 6 held" },
        { step: "ACCOUNT + REV 2", log: "One canceled report finalizes the old child, completes replace, routes revision 2, and prepares its child in the same core apply and journal batch.", parentId: "#20 · revision 2", parentState: "ACTIVE", parentCopy: "new revision owns 8 @ 98", parentClass: "", children: [{ label: "Old child · B · 6 @ 99", state: "CANCELED", copy: "revision 1 · risk 0", className: "terminal" }, { label: "New child · B · 8 @ 98", state: "PREPARED", copy: "SXN2-2-3 · risk 8", className: "" }], risk: 8, riskCopy: "No observable risk-zero pause: revision 2 is prepared atomically with old-child accounting.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ChildTerminal · RouteDecision · ChildPrepared", effect: "new effect queued after same durable batch", status: "Revision 2 prepared atomically · risk 8 held" },
        { step: "POLL NEW", log: "A later poll commits DispatchStarted for revision 2 before releasing its new-order effect.", children: [{ label: "Old child · B · 6 @ 99", state: "CANCELED", copy: "revision 1 · risk 0", className: "terminal" }, { label: "New child · B · 8 @ 98", state: "DISPATCH STARTED", copy: "SXN2-2-3 · risk 8", className: "" }], event: "poll_egress(B)", record: "DispatchStarted · new attempt", effect: "FIX New revision 2", status: "Revision 2 dispatch started · risk 8 held" },
        { step: "FILL", log: "A definite fill of the new child completes revision 2 and releases its reservation.", parentState: "FILLED", parentCopy: "revision 2 · filled 8 / 8", parentClass: "terminal", children: [{ label: "Old child · B · 6 @ 99", state: "CANCELED", copy: "revision 1 · risk 0", className: "terminal" }, { label: "New child · B · 8 @ 98", state: "FILLED", copy: "revision 2 · risk 0", className: "terminal" }], risk: 0, riskCopy: "Terminal accounting released revision 2 risk.", event: "ExecutionReportEvent", record: "ExecutionOutcome · ParentTerminal", effect: "ParentReport · FILLED", status: "Replace complete · filled · risk 0" },
      ],
      expected: { parentState: "FILLED", risk: 0 },
    },
    unknown: {
      label: "Unknown dispatch",
      initial: lifeInitial({ parentId: "#20 · revision 1", parentState: "ACTIVE", parentCopy: "child A dispatch attempt 1 is in flight", children: [{ label: "Child #1 · A · 5 @ 100", state: "DISPATCH STARTED", copy: "SXN1-1-1 · risk 5", className: "" }], risk: 5, riskTotal: 5, riskCopy: "Durable child owns all five units.", event: "DispatchStarted", record: "DispatchStarted · attempt 1", effect: "wire outcome pending", receipt: true, receiptText: "Submit accepted durably", status: "Ready · inject UNKNOWN local result" }),
      frames: [
        { step: "UNKNOWN", log: "The transport cannot prove the new order absent or locally accepted.", parentState: "SUSPENDED", parentCopy: "quantity ownership requires external reconciliation", parentClass: "ambiguous", children: [{ label: "Child #1 · A · 5 @ 100", state: "AMBIGUOUS", copy: "wire SXN1-1-1 · risk 5 HELD", className: "ambiguous" }], risk: 5, riskCopy: "AMBIGUOUS is nonterminal; reservation remains held.", riskClass: "danger", event: "LocalDispatchEvent · UNKNOWN", record: "DispatchResult · state image", effect: "none · no reroute", status: "Suspended · ambiguous · risk 5 held" },
        { step: "OWNED", log: "SORX emits no child on another venue. A later stale result cannot silently transfer ownership.", event: "reconciliation required", record: "ownership preserved", effect: "new routing for these 5 units prohibited", status: "No reallocation · absence was not proved" },
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
      copy.textContent = "no quantity has been prepared";
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
    renderLog(byId("life-log"), scenario.frames, frameIndex, "Run or step to follow the journal and wire boundary.");
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
      heading: "No durable command, no released effect",
      copy: "The caller may retry because SORX did not report durable acceptance.",
    },
    prepared: {
      badge: "DEFINITELY UNSENT",
      badgeClass: "committed",
      heading: "Prepared child is durable; the wire effect has not escaped",
      copy: "Recovery may retry with the same durable wire ID. It advances the dispatch attempt, not the order identity.",
    },
    wire: {
      badge: "OWNERSHIP UNCERTAIN",
      badgeClass: "ambiguous",
      heading: "Absence can no longer be proved",
      copy: "An unresolved child becomes AMBIGUOUS, the parent is suspended, and risk stays held. No other venue receives that quantity.",
    },
    terminal: {
      badge: "DEFINITE OUTCOME",
      badgeClass: "eligible",
      heading: "Terminal accounting releases reservation",
      copy: "Fill, cancel, or definite reject is applied once, persisted, deduped, and reflected in the parent outcome.",
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
      book: "no publication yet",
      action: "waiting for a proven snapshot cut",
      status: "Ready",
    }, overrides || {});
  }

  var FEED_SCENARIOS = {
    stitch: {
      label: "Snapshot + stitch",
      initial: feedInitial(),
      frames: [
        { step: "BUFFER", log: "Buffer session 44 live packet at sequence 10 while the snapshot is in flight. The session binds, but next_sequence remains at its initial value.", session: "44", next: "1", buffered: "1", status: "Buffered live seq 10 · next remains 1" },
        { step: "CUT 10", log: "Install snapshot bound to session 44, cut 10, and live high-water 13; replay buffered events through 11.", state: "STITCHING", cut: "10", next: "12", highWater: "13", buffered: "0", book: "bid 100 × 10 · asks 105 × 9 / 106 × 4", action: "STITCHING · next 12 must reach high-water 13", status: "Snapshot installed · catching up" },
        { step: "SEQ 12", log: "Apply validated book-neutral sequence 12. next_sequence advances to the proven high-water.", next: "13", action: "caught up · complete_stitch may now open LIVE", status: "Caught up · next 13" },
        { step: "LIVE", log: "Complete stitch and publish an owned LIVE BookView generation.", state: "LIVE", action: "LIVE · publication generation advanced", status: "LIVE · continuity proven" },
      ],
      expected: { state: "LIVE", next: "13" },
    },
    gap: {
      label: "Sequence gap recovery",
      initial: feedInitial({ state: "LIVE", session: "77", cut: "9", next: "10", highWater: "10", book: "bid 100 × 12 · ask 105 × 9", action: "LIVE · waiting for sequence 10", status: "Ready · LIVE session 77" }),
      frames: [
        { step: "SEQ 10", log: "Apply the expected session 77 packet at sequence 10. The snapshot high-water remains 10.", next: "11", highWater: "10", action: "LIVE · next expected 11", status: "LIVE · next 11 · high-water 10" },
        { step: "GAP", log: "Receive future sequence 12 while 11 is missing. Publish RECOVERING before requesting a fresh snapshot.", state: "RECOVERING", buffered: "1", book: "non-LIVE BookView · prior depth retained for inspection only", action: "SEQUENCE GAP · REQUEST FRESH SNAPSHOT", status: "RECOVERING · LIVE withdrawn" },
        { step: "CUT 12", log: "Install a fresh snapshot at cut 12 / high-water 13 and replay the buffered future packet.", state: "STITCHING", cut: "12", next: "13", highWater: "13", buffered: "0", book: "fresh snapshot + validated buffered seq 12", action: "STITCHING · caught up to 13", status: "Fresh cut installed · accounting continuity restored" },
        { step: "LIVE", log: "Complete stitch and publish a new LIVE generation. Routing may consider this venue again.", state: "LIVE", action: "LIVE · new recovery and publication generations", status: "LIVE again · proven cut" },
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
    renderLog(byId("feed-log"), scenario.frames, frameIndex, "Run or step to follow snapshot and live continuity.");
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
        ? "AMBIGUOUS child retains risk; accounting is not recovery-consistent"
        : (open ? "new parents may enter deterministic routing" : "no new parent may route");

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
          ? "Unresolved branch selected · routing will remain closed"
          : "Resolved branch selected · 0 / 5 gates satisfied");
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
    markCurrent(initialHash || "overview");

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

