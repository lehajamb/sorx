const fs = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");

class FakeClassList {
  constructor(owner) {
    this.owner = owner;
    this.names = new Set();
  }
  toggle(name, force) {
    const active = force === undefined ? !this.names.has(name) : Boolean(force);
    if (active) this.names.add(name);
    else this.names.delete(name);
    return active;
  }
  contains(name) {
    return this.names.has(name);
  }
  fromString(value) {
    this.names = new Set(String(value || "").split(/\s+/).filter(Boolean));
  }
  toString() {
    return Array.from(this.names).join(" ");
  }
}

class FakeElement {
  constructor(tagName = "div", id = "") {
    this.tagName = tagName.toUpperCase();
    this.id = id;
    this.dataset = {};
    this.attributes = {};
    this.style = {};
    this.children = [];
    this.parentElement = null;
    this.listeners = {};
    this.textContent = "";
    this.value = "";
    this.disabled = false;
    this.tabIndex = 0;
    this.scrollTop = 0;
    this.classList = new FakeClassList(this);
  }
  get className() {
    return this.classList.toString();
  }
  set className(value) {
    this.classList.fromString(value);
  }
  get scrollHeight() {
    return this.children.length * 20;
  }
  setAttribute(name, value) {
    this.attributes[name] = String(value);
    if (name.startsWith("data-")) {
      const key = name.slice(5).replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
      this.dataset[key] = String(value);
    }
  }
  getAttribute(name) {
    return Object.prototype.hasOwnProperty.call(this.attributes, name) ? this.attributes[name] : null;
  }
  removeAttribute(name) {
    delete this.attributes[name];
  }
  addEventListener(type, listener) {
    (this.listeners[type] ||= []).push(listener);
  }
  async fire(type, event = {}) {
    event.preventDefault ||= () => {};
    event.target ||= this;
    event.currentTarget ||= this;
    const results = (this.listeners[type] || []).map((listener) => listener(event));
    await Promise.all(results);
  }
  click() {
    return this.fire("click");
  }
  focus() {
    FakeElement.focused = this;
  }
  appendChild(item) {
    if (item && typeof item === "object") item.parentElement = this;
    this.children.push(item);
    return item;
  }
  append(...items) {
    items.forEach((item) => this.appendChild(item));
  }
  replaceChildren(...items) {
    this.children = [];
    this.append(...items);
  }
  querySelector(selector) {
    const tag = selector.toUpperCase();
    const visit = (node) => {
      if (!node || typeof node !== "object") return null;
      if (node.tagName === tag) return node;
      for (const child of node.children || []) {
        const found = visit(child);
        if (found) return found;
      }
      return null;
    };
    return visit(this);
  }
}

function element(tag = "div", id = "") {
  return new FakeElement(tag, id);
}

const html = fs.readFileSync("doc/index.html", "utf8");
const script = fs.readFileSync("doc/sorx-docs.js", "utf8");
const elements = new Map();
for (const match of html.matchAll(/<([a-z0-9]+)[^>]*\bid="([^"]+)"/gi)) {
  assert(!elements.has(match[2]), "duplicate HTML id #" + match[2]);
  elements.set(match[2], element(match[1], match[2]));
}

function requireElement(id) {
  const value = elements.get(id);
  assert(value, "missing fixture element #" + id);
  return value;
}

for (const match of html.matchAll(/\bhref="#([^"]+)"/g)) {
  requireElement(match[1]);
}

function dataButton(attribute, key, active = false) {
  const button = element("button");
  button.setAttribute(attribute, key);
  button.setAttribute("aria-pressed", String(active));
  if (active) button.classList.toggle("active", true);
  return button;
}

function scenarioKeys(attribute, expected) {
  const keys = Array.from(html.matchAll(new RegExp("<button\\b[^>]*\\b" + attribute + '=\"([^\"]+)\"', "gi")), (match) => match[1]);
  assert.equal(new Set(keys).size, keys.length, "duplicate HTML " + attribute + " button");
  assert.deepEqual([...keys].sort(), [...expected].sort(), "HTML buttons for " + attribute);
  return keys;
}

function stateAt(scenario, frameIndex = scenario.frames.length - 1) {
  return Object.assign({}, scenario.initial, ...scenario.frames.slice(0, frameIndex + 1));
}

const planeButtons = ["management", "control", "forwarding"].map((key, index) => {
  const button = requireElement("plane-tab-" + key);
  button.dataset.plane = key;
  button.setAttribute("aria-selected", String(index === 0));
  return button;
});
const routeKeys = scenarioKeys("data-route-scenario", ["sweep", "new-prices", "quantity-cap", "limit", "venue-cap", "passive", "marketable", "withdrawn"]);
const routeButtons = routeKeys.map((key, index) => dataButton("data-route-scenario", key, index === 0));
const lifeKeys = scenarioKeys("data-life-scenario", ["fill", "partial", "cancel", "replace", "unknown"]);
const lifeButtons = lifeKeys.map((key, index) => dataButton("data-life-scenario", key, index === 0));
const feedKeys = scenarioKeys("data-feed-scenario", ["stitch", "gap"]);
const feedButtons = feedKeys.map((key, index) => dataButton("data-feed-scenario", key, index === 0));
const crashButtons = scenarioKeys("data-crash-point", ["before", "prepared", "wire", "terminal"]).map((key, index) => dataButton("data-crash-point", key, index === 0));
const recoveryButtons = scenarioKeys("data-recovery-outcome", ["resolved", "ambiguous"]).map((key, index) => dataButton("data-recovery-outcome", key, index === 0));
const feedStates = ["SNAPSHOTTING", "STITCHING", "LIVE", "RECOVERING"].map((key, index) => {
  const node = element("div");
  node.dataset.feedState = key;
  if (index === 0) node.setAttribute("aria-current", "step");
  return node;
});
const recoveryRows = Array.from({ length: 5 }, () => {
  const row = element("li");
  row.append(element("b"));
  return row;
});

requireElement("route-summary").replaceChildren(element("span"), element("strong"), element("small"));
requireElement("life-receipt").replaceChildren(element("span"), element("strong"), element("small"));
requireElement("routing-gate").replaceChildren(element("span"), element("strong"), element("small"));
requireElement("route-insight").append(element("p"));
const lifeParentCard = element("div");
lifeParentCard.append(requireElement("life-parent-state"));
requireElement("route-speed").value = "4";

const selectorMap = new Map([
  [".plane-tab", planeButtons],
  ["[data-route-scenario]", routeButtons],
  ["[data-life-scenario]", lifeButtons],
  ["[data-feed-scenario]", feedButtons],
  ["[data-crash-point]", crashButtons],
  ["[data-recovery-outcome]", recoveryButtons],
  ["[data-feed-state]", feedStates],
  ["#recovery-gates li", recoveryRows],
  [".navlinks a[href^=\"#\"]", []],
]);

const document = {
  documentElement: element("html"),
  getElementById: (id) => elements.get(id) || null,
  querySelectorAll: (selector) => selectorMap.get(selector) || [],
  createElement: (tag) => element(tag),
};
const reducedMotion = process.env.SORX_DOCS_REDUCED_MOTION !== "0";
const window = {
  matchMedia: () => ({ matches: reducedMotion }),
  setTimeout,
  location: { hash: "" },
};
const context = vm.createContext({ window, document, console, setTimeout, clearTimeout, Promise, Object, Array, Math, String, Number, Boolean });
vm.runInContext(script, context, { filename: "doc/sorx-docs.js" });

assert.equal(document.documentElement.dataset.scenariosValid, "true");
assert.equal(window.SorxDocs.reducedMotion, reducedMotion, "playback uses the requested motion preference");
assert.deepEqual({ ...window.SorxDocs.validation }, { routes: true, lifecycle: true, feed: true });
assert.deepEqual(Object.keys(window.SorxDocs.routeScenarios).sort(), [...routeKeys].sort(), "every routing fixture has an HTML control");
assert.deepEqual(Object.keys(window.SorxDocs.lifecycleScenarios).sort(), [...lifeKeys].sort(), "every lifecycle fixture has an HTML control");
assert.deepEqual(Object.keys(window.SorxDocs.feedScenarios).sort(), [...feedKeys].sort(), "every feed fixture has an HTML control");

async function click(button) {
  await button.click();
}

async function runAll() {
  const expectedRoutes = {
    sweep: ["10", "0", "NONE", "10", 2],
    "new-prices": ["10", "0", "NONE", "10", 1],
    "quantity-cap": ["10", "0", "NONE", "10", 2],
    limit: ["4", "6", "PARENT LIMIT REACHED", "4", 1],
    "venue-cap": ["4", "6", "MAXIMUM VENUE COUNT", "4", 1],
    passive: ["10", "0", "NONE", "10", 1],
    marketable: ["0", "10", "PASSIVE FULL QUANTITY UNAVAILABLE", "0", 0],
    withdrawn: ["3", "0", "NONE", "3", 1],
  };
  for (let index = 0; index < routeKeys.length; index += 1) {
    const key = routeKeys[index];
    await click(routeButtons[index]);
    await click(requireElement("route-run"));
    const actualChildren = requireElement("route-children").children.filter((child) => child && child.className === "child-card").length;
    assert.deepEqual([
      requireElement("route-allocated").textContent,
      requireElement("route-remainder").textContent,
      requireElement("route-remainder-reason").textContent,
      requireElement("route-risk").textContent,
      actualChildren,
    ], expectedRoutes[key], "route " + key);
    assert.equal(requireElement("route-run").getAttribute("aria-busy"), "false");

    const scenario = window.SorxDocs.routeScenarios[key];
    for (let frameIndex = -1; frameIndex < scenario.frames.length; frameIndex += 1) {
      const state = stateAt(scenario, frameIndex);
      assert.equal(state.allocated + state.remainder, scenario.order.quantity, key + " conserves requested quantity at frame " + frameIndex);
      assert(state.allocated >= 0 && state.remainder >= 0, key + " has no negative quantity");
      assert.equal(new Set(state.children.map((child) => child.venue)).size, state.children.length, key + " has at most one child per venue");
      const maxVenues = Number(String(scenario.order.maxVenues).match(/\d+/)?.[0]);
      assert(Number.isInteger(maxVenues) && maxVenues > 0, key + " declares a maximum venue count");
      assert(state.children.length <= maxVenues, key + " respects maximum venue count");
      for (const child of state.children) {
        assert(child.quantity > 0, key + " never creates a zero-sized child");
        assert(child.price <= Number(scenario.order.limit), key + " respects the buy parent's price limit");
        const cap = scenario.venues[child.venue].allocationCap;
        if (cap !== undefined) assert(child.quantity <= cap, key + " respects " + child.venue + " quantity cap");
      }
    }
    const final = stateAt(scenario);
    assert.equal(final.children.reduce((sum, child) => sum + child.quantity, 0), final.allocated, key + " child quantities equal planned allocation");
  }

  const baseline = window.SorxDocs.routeScenarios.sweep;
  const changedPrices = window.SorxDocs.routeScenarios["new-prices"];
  for (const field of ["quantity", "intent", "sideQuantity", "limit", "maxVenues", "ranking"]) {
    assert.equal(changedPrices.order[field], baseline.order[field], "new-prices keeps the same order rule: " + field);
  }
  for (const venue of ["A", "B"]) {
    for (const field of ["priority", "health", "allocationCap"]) {
      assert.equal(changedPrices.venues[venue][field], baseline.venues[venue][field], "new-prices keeps " + venue + " " + field);
    }
  }
  assert.notEqual(changedPrices.venues.B.ask, baseline.venues.B.ask, "market data changes while configured rules stay fixed");
  assert.deepEqual(Array.from(stateAt(baseline).children, (child) => [child.venue, child.quantity]), [["A", 4], ["B", 6]]);
  assert.deepEqual(Array.from(stateAt(changedPrices).children, (child) => [child.venue, child.quantity]), [["B", 10]]);
  const quantityCap = window.SorxDocs.routeScenarios["quantity-cap"];
  assert.equal(quantityCap.venues.B.allocationCap, 6, "quantity-cap declares B's six-share cap");
  assert.deepEqual(Array.from(stateAt(quantityCap).children, (child) => [child.venue, child.quantity]), [["B", 6], ["A", 4]], "cheaper B receives only its permitted six shares");

  await click(routeButtons[0]);
  await click(requireElement("route-reset"));
  await click(requireElement("route-step"));
  assert.equal(requireElement("route-log").children.length, 1);
  assert.equal(requireElement("route-status").textContent, stateAt(baseline, 0).status, "step displays the first routing event");
  await click(requireElement("route-reset"));
  assert.equal(requireElement("route-allocated").textContent, "0");

  const runningRoute = requireElement("route-run").click();
  await new Promise((resolve) => setTimeout(resolve, 5));
  await click(requireElement("route-reset"));
  await runningRoute;
  assert.equal(requireElement("route-allocated").textContent, "0", "reset cancels active route run");

  const lifeExpected = {
    fill: ["FILLED", "0 / 10 held"],
    partial: ["CANCELED", "0 / 10 held"],
    cancel: ["CANCELED", "0 / 5 held"],
    replace: ["FILLED", "0 / 8 held"],
    unknown: ["SUSPENDED", "5 / 5 held"],
  };
  for (let index = 0; index < lifeKeys.length; index += 1) {
    await click(lifeButtons[index]);
    await click(requireElement("life-run"));
    assert.deepEqual([
      requireElement("life-parent-state").textContent,
      requireElement("life-risk-label").textContent,
    ], lifeExpected[lifeKeys[index]], "lifecycle " + lifeKeys[index]);
  }
  assert.match(requireElement("life-risk-fill").getAttribute("aria-valuetext"), /5 of 5 units held/);

  const partial = window.SorxDocs.lifecycleScenarios.partial;
  const originalChildren = Array.from(partial.initial.children, (child) => child.label);
  for (let frameIndex = -1; frameIndex < partial.frames.length; frameIndex += 1) {
    const state = stateAt(partial, frameIndex);
    assert.deepEqual(Array.from(state.children, (child) => child.label), originalChildren, "partial fills retain the same two venue children");
    assert.equal(state.children.reduce((sum, child) => sum + child.risk, 0), state.risk, "parent risk equals held child reservations");
    assert.equal(state.children.reduce((sum, child) => sum + child.filled, 0), state.filled, "parent filled quantity equals confirmed child fills");
    const leaves = state.children.reduce((sum, child) => sum + child.leaves, 0);
    assert.equal(state.filled + state.unfilled + leaves, state.parentQuantity, "filled + unfilled + working conserves the partial-fill parent");
    for (const child of state.children) {
      assert.equal(child.filled + child.leaves + (child.unfilled || 0), child.quantity, "partial-fill child quantity is conserved");
    }
  }
  const partialIndex = partial.frames.findIndex((frame) => frame.step === "B PARTIAL");
  assert(partialIndex >= 0, "partial-fill preset includes B's nonterminal partial fill");
  const bPartial = stateAt(partial, partialIndex);
  assert.equal(bPartial.filled, 6);
  assert.equal(bPartial.children[1].filled, 2);
  assert.equal(bPartial.children[1].leaves, 4);
  assert.equal(bPartial.children[1].risk, 6, "original core retains B's full reservation until terminal accounting");
  assert.equal(bPartial.risk, 6);
  const terminalPartial = stateAt(partial);
  assert.equal(terminalPartial.filled, 6);
  assert.equal(terminalPartial.unfilled, 4);
  assert.equal(terminalPartial.risk, 0, "terminal cancellation releases the full held reservation");
  assert.equal(terminalPartial.children[1].unfilled, 4);
  assert.equal(terminalPartial.children[1].risk, 0);

  await click(lifeButtons[lifeKeys.indexOf("partial")]);
  for (let frameIndex = 0; frameIndex < partial.frames.length; frameIndex += 1) {
    await click(requireElement("life-step"));
    assert.equal(requireElement("life-risk-label").textContent, stateAt(partial, frameIndex).risk + " / 10 held", "stepping partial fills displays the held reservation");
  }
  assert.equal(requireElement("life-parent-state").textContent, "CANCELED");
  assert.match(requireElement("life-parent-copy").textContent, /6 \/ 10 filled.*4 \/ 10 unfilled/);

  for (let index = 0; index < feedKeys.length; index += 1) {
    await click(feedButtons[index]);
    await click(requireElement("feed-run"));
    assert.equal(requireElement("feed-next").textContent, "13");
    assert.equal(feedStates.filter((node) => node.getAttribute("aria-current") === "step").length, 1);
    assert.equal(feedStates.find((node) => node.getAttribute("aria-current") === "step").dataset.feedState, "LIVE");
  }

  await planeButtons[0].fire("keydown", { key: "ArrowRight", preventDefault() {} });
  assert.equal(planeButtons[1].getAttribute("aria-selected"), "true");
  assert.equal(FakeElement.focused, planeButtons[1]);
  await planeButtons[1].fire("keydown", { key: "End", preventDefault() {} });
  assert.equal(planeButtons[2].getAttribute("aria-selected"), "true");

  for (const button of crashButtons) {
    await click(button);
    assert.equal(requireElement("crash-detail").children.length, 3);
  }

  await click(recoveryButtons[0]);
  await click(requireElement("recovery-run"));
  assert(requireElement("routing-gate").classList.contains("open"));
  assert.match(requireElement("recovery-status").textContent, /routing enabled/);

  await click(recoveryButtons[1]);
  await click(requireElement("recovery-run"));
  assert(requireElement("routing-gate").classList.contains("closed"));
  assert.match(requireElement("recovery-status").textContent, /AMBIGUOUS risk held/);
  assert(recoveryRows[3].classList.contains("blocked"));
  assert.equal(requireElement("recovery-run").disabled, true);

  await click(recoveryButtons[0]);
  for (let index = 0; index < 5; index += 1) await click(requireElement("recovery-step"));
  assert(requireElement("routing-gate").classList.contains("open"));

  assert.equal(requireElement("route-meter-fill").getAttribute("aria-valuenow"), "0");
  await click(routeButtons[0]);
  await click(requireElement("route-run"));
  assert.equal(requireElement("route-meter-fill").getAttribute("aria-valuenow"), "10");
  assert.equal(requireElement("route-a-depth").getAttribute("aria-valuenow"), "4");
  assert.equal(requireElement("route-b-depth").getAttribute("aria-valuenow"), "6");

  console.log("Interaction smoke passed (" + (reducedMotion ? "reduced" : "normal") + " motion): 8 routes, 5 lifecycles, quantity conservation, price and venue caps, unchanged rules with updated quotes, partial-fill risk accounting, 2 feed paths, 4 crash points, plane keyboard navigation, reset cancellation, and both recovery outcomes.");
}

runAll().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
