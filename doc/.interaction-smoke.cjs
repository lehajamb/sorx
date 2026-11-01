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
  elements.set(match[2], element(match[1], match[2]));
}

function requireElement(id) {
  const value = elements.get(id);
  assert(value, "missing fixture element #" + id);
  return value;
}

function dataButton(attribute, key, active = false) {
  const button = element("button");
  button.setAttribute(attribute, key);
  button.setAttribute("aria-pressed", String(active));
  if (active) button.classList.toggle("active", true);
  return button;
}

const planeButtons = ["management", "control", "forwarding"].map((key, index) => {
  const button = requireElement("plane-tab-" + key);
  button.dataset.plane = key;
  button.setAttribute("aria-selected", String(index === 0));
  return button;
});
const routeKeys = ["sweep", "limit", "venue-cap", "passive", "marketable", "withdrawn"];
const routeButtons = routeKeys.map((key, index) => dataButton("data-route-scenario", key, index === 0));
const lifeKeys = ["fill", "cancel", "replace", "unknown"];
const lifeButtons = lifeKeys.map((key, index) => dataButton("data-life-scenario", key, index === 0));
const feedKeys = ["stitch", "gap"];
const feedButtons = feedKeys.map((key, index) => dataButton("data-feed-scenario", key, index === 0));
const crashButtons = ["before", "prepared", "wire", "terminal"].map((key, index) => dataButton("data-crash-point", key, index === 0));
const recoveryButtons = ["resolved", "ambiguous"].map((key, index) => dataButton("data-recovery-outcome", key, index === 0));
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
const window = {
  matchMedia: () => ({ matches: true }),
  setTimeout,
  location: { hash: "" },
};
const context = vm.createContext({ window, document, console, setTimeout, clearTimeout, Promise, Object, Array, Math, String, Number, Boolean });
vm.runInContext(script, context, { filename: "doc/sorx-docs.js" });

assert.equal(document.documentElement.dataset.scenariosValid, "true");
assert.deepEqual({ ...window.SorxDocs.validation }, { routes: true, lifecycle: true, feed: true });

async function click(button) {
  await button.click();
}

async function runAll() {
  const expectedRoutes = {
    sweep: ["10", "0", "NONE", "10", 2],
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
  }

  await click(routeButtons[0]);
  await click(requireElement("route-reset"));
  await click(requireElement("route-step"));
  assert.equal(requireElement("route-log").children.length, 1);
  assert.match(requireElement("route-status").textContent, /Pinned/);
  await click(requireElement("route-reset"));
  assert.equal(requireElement("route-allocated").textContent, "0");

  const runningRoute = requireElement("route-run").click();
  await new Promise((resolve) => setTimeout(resolve, 5));
  await click(requireElement("route-reset"));
  await runningRoute;
  assert.equal(requireElement("route-allocated").textContent, "0", "reset cancels active route run");

  const lifeExpected = {
    fill: ["FILLED", "0 / 10 held"],
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

  console.log("Interaction smoke passed: 6 routes, 4 lifecycles, 2 feed paths, 4 crash points, plane keyboard navigation, reset cancellation, and both recovery outcomes.");
}

runAll().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
