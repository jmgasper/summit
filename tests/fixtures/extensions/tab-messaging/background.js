"use strict";
const frames = new Map();
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const cases = {};
const check = (name, value) => { cases[name] = !!value; if (!value) throw new Error(name); };
const post = (phase, extra = {}) => fetch(CONTROL + "/report/" + phase, {
    method: "POST", headers: {"Content-Type": "application/json"},
    body: JSON.stringify({phase, runtimeId: browser.runtime.id, cases: {...cases}, ...extra})
});
const pause = () => new Promise(resolve => setTimeout(resolve, 50));
const until = async predicate => {
    const deadline = Date.now() + 60000;
    while (!predicate()) { if (Date.now() > deadline) throw new Error("Timed out waiting for content frames"); await pause(); }
};
const reject = async (name, operation) => {
    let error;
    try { await operation(); } catch (value) { error = value; }
    check(name, error && typeof error.message === "string" && error.message.length > 0);
};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.kind !== "ready") return;
    frames.set(message.label, sender);
    sendResponse({acknowledged: message.label});
});
async function run() {
    await until(() => frames.size === 4);
    const isolationDeadline = Date.now() + 15000;
    let isolationReady = false;
    while (!isolationReady) {
        isolationReady = (await (await fetch(CONTROL + "/isolation-ready")).json()).ready;
        if (Date.now() > isolationDeadline) throw new Error("Other extension listeners did not register");
        if (!isolationReady) await pause();
    }
    check("other-extension-listening", isolationReady);
    const a = frames.get("a/main"), ac = frames.get("a/child"), b = frames.get("b/main"), bc = frames.get("b/child");
    check("real-tab-identifiers", Number.isInteger(a.tab.id) && a.tab.id !== b.tab.id && ac.tab.id === a.tab.id && bc.tab.id === b.tab.id);
    check("real-frame-identifiers", a.frameId === 0 && b.frameId === 0 && ac.frameId > 0 && bc.frameId > 0 && ac.frameId !== bc.frameId);
    check("real-document-identifiers", [a,ac,b,bc].every(sender => uuid.test(sender.documentId)) && new Set([a,ac,b,bc].map(sender => sender.documentId)).size === 4);
    check("content-sender-identity", [a,ac,b,bc].every(sender => sender.id === browser.runtime.id));
    const liveTabIDs = [a.tab.id, b.tab.id].sort((x, y) => x - y);
    const sameTabs = (tabs, ids) => JSON.stringify(tabs.map(tab => tab.id).sort((x, y) => x - y)) === JSON.stringify(ids);
    const ordinary = await browser.tabs.query({});
    check("discarded-omitted-query", sameTabs(ordinary, liveTabIDs));
    check("discarded-query-metadata", ordinary.every(tab => tab.discarded === false));
    check("discarded-sender-metadata", [a, ac, b, bc].every(sender => sender.tab.discarded === false));
    check("discarded-get-metadata", (await browser.tabs.get(a.tab.id)).discarded === false);
    check("discarded-false-query", sameTabs(await browser.tabs.query({discarded: false}), liveTabIDs));
    check("discarded-true-query", (await browser.tabs.query({discarded: true})).length === 0);
    check("discarded-active-query", sameTabs(await browser.tabs.query({discarded: false, active: true}), [b.tab.id]));
    check("discarded-inactive-query", sameTabs(await browser.tabs.query({discarded: false, active: false}), [a.tab.id]));
    check("discarded-window-query", sameTabs(await browser.tabs.query({discarded: false, windowId: a.tab.windowId}), liveTabIDs));
    check("discarded-combined-query", sameTabs(await browser.tabs.query({discarded: false, active: false, currentWindow: true,
        url: "http://127.0.0.1" + new URL(a.url).pathname}), [a.tab.id]));
    check("discarded-true-combined-query", (await browser.tabs.query({discarded: true, active: false, windowId: a.tab.windowId})).length === 0);
    const callbackTabs = await new Promise((resolve, fail) => chrome.tabs.query({discarded: false}, tabs => {
        const error = chrome.runtime.lastError;
        error ? fail(new Error(error.message)) : resolve(tabs);
    }));
    check("discarded-chrome-callback", sameTabs(callbackTabs, liveTabIDs) && callbackTabs.every(tab => tab.discarded === false));
    // Chromium and Firefox normalize null/undefined optional dictionary fields as omitted.
    check("discarded-null-query", sameTabs(await browser.tabs.query({discarded: null}), liveTabIDs));
    check("discarded-undefined-query", sameTabs(await browser.tabs.query({discarded: undefined}), liveTabIDs));
    const nullCallbackTabs = await new Promise((resolve, fail) => chrome.tabs.query({discarded: null}, tabs => {
        const error = chrome.runtime.lastError;
        error ? fail(new Error(error.message)) : resolve(tabs);
    }));
    check("discarded-chrome-null-callback", sameTabs(nullCallbackTabs, liveTabIDs));
    for (const [label, invalid] of [["zero", 0], ["one", 1], ["string", "false"], ["array", []], ["object", {}]])
        await reject("discarded-invalid-" + label, () => browser.tabs.query({discarded: invalid}));
    const payload = {nested: [false, null, 0, "雪"], text: "message"};
    const send = (tab, tag, options, kind = "probe") => browser.tabs.sendMessage(tab, {kind, tag, payload}, options);
    let reply = await send(a.tab.id, "main", {frameId: 0});
    check("promise-main-frame", reply.label === "a/main" && JSON.stringify(reply.payload) === JSON.stringify(payload));
    check("background-sender-identity", reply.id === browser.runtime.id && reply.sender.id === browser.runtime.id && reply.sender.url === location.href && !reply.sender.tab && uuid.test(reply.sender.documentId));
    reply = await send(a.tab.id, "child", {frameId: ac.frameId});
    check("child-frame", reply.label === "a/child");
    reply = await send(a.tab.id, "document", {documentId: ac.documentId});
    check("document-target", reply.label === "a/child");
    reply = await send(a.tab.id, "both", {frameId: ac.frameId, documentId: ac.documentId});
    check("matching-frame-and-document", reply.label === "a/child");
    reply = await send(b.tab.id, "other-tab", {frameId: 0});
    check("other-tab-target", reply.label === "b/main");
    reply = await new Promise((resolve, fail) => chrome.tabs.sendMessage(a.tab.id, {kind: "probe", tag: "callback", payload}, {frameId: 0}, value => {
        if (chrome.runtime.lastError) fail(new Error(chrome.runtime.lastError.message)); else resolve(value);
    }));
    check("chrome-callback", reply.label === "a/main");
    reply = await send(a.tab.id, "later", {frameId: 0}, "later");
    check("asynchronous-send-response", reply.label === "a/main");
    reply = await send(a.tab.id, "promise", {frameId: ac.frameId}, "promise");
    check("listener-promise", reply.label === "a/child");
    check("first-response-wins", await send(a.tab.id, "twice", {frameId: 0}, "twice") === "first");
    for (const [name, value] of [["false", false], ["zero", 0], ["empty", ""], ["null", null]])
        check("reply-" + name, await browser.tabs.sendMessage(a.tab.id, {kind: "value", tag: name, payload: value}, {frameId: 0}) === value);
    check("undefined-response", await send(a.tab.id, "undefined", {frameId: 0}, "undefined") === undefined);
    check("listener-without-response", await send(a.tab.id, "none", {frameId: 0}, "none") === undefined);
    for (const [name, value] of [["null", null], ["false", false], ["number", 42], ["string", "雪"], ["array", [true, null, "雪"]]]) {
        reply = await browser.tabs.sendMessage(a.tab.id, value, {frameId: 0});
        check("message-" + name, reply.label === "a/main" && JSON.stringify(reply.value) === JSON.stringify(value));
    }
    const cycle = {}; cycle.self = cycle;
    await reject("chrome-cyclic-message", () => chrome.tabs.sendMessage(a.tab.id, cycle, {frameId: 0}));
    await reject("function-message", () => browser.tabs.sendMessage(a.tab.id, () => {}, {frameId: 0}));
    reply = await browser.tabs.sendMessage(a.tab.id, {kind: "probe", tag: "broadcast", payload});
    check("all-frames-response", ["a/main", "a/child"].includes(reply.label));
    const state = async (tab, frame) => browser.tabs.sendMessage(tab, {kind: "state"}, {frameId: frame});
    check("broadcast-main-delivery", (await state(a.tab.id, 0)).includes("broadcast"));
    check("broadcast-child-delivery", (await state(a.tab.id, ac.frameId)).includes("broadcast"));
    check("frame-isolation", !(await state(a.tab.id, ac.frameId)).includes("main") && !(await state(a.tab.id, 0)).includes("child"));
    check("tab-isolation", JSON.stringify(await state(b.tab.id, 0)) === '["other-tab"]' && (await state(b.tab.id, bc.frameId)).length === 0);
    await reject("missing-tab", () => send(999999999, "invalid", {frameId: 0}));
    await reject("missing-frame", () => send(a.tab.id, "invalid", {frameId: 999999999}));
    await reject("missing-document", () => send(a.tab.id, "invalid", {documentId: "12345678-1234-4234-8234-123456789abc"}));
    await reject("document-in-other-tab", () => send(a.tab.id, "invalid", {documentId: b.documentId}));
    await reject("conflicting-frame-document", () => send(a.tab.id, "invalid", {frameId: 0, documentId: ac.documentId}));
    for (const [name, value] of [["negative", -1], ["fraction", .5], ["nan", NaN], ["infinity", Infinity]])
        await reject("invalid-tab-" + name, () => send(value, "invalid", {}));
    for (const [name, options] of [["string", "x"], ["array", []], ["frame-negative", {frameId:-1}], ["frame-fraction", {frameId:.5}], ["frame-string", {frameId:"0"}], ["document-malformed", {documentId:"bad"}], ["document-number", {documentId:1}], ["unsupported", {unknown:true}]])
        await reject("invalid-options-" + name, () => send(a.tab.id, "invalid", options));
    let lastError, callbackValue;
    await new Promise(resolve => chrome.tabs.sendMessage(a.tab.id, {kind: "probe", tag: "invalid"}, {frameId: 999999999}, value => {
        lastError = chrome.runtime.lastError;
        callbackValue = value;
        resolve();
    }));
    check("callback-error-value", callbackValue === undefined);
    check("callback-last-error", lastError && typeof lastError.message === "string" && !chrome.runtime.lastError);
    await post("initial", {frames: Object.fromEntries(frames)});
    await until(() => frames.get("a/main").documentId !== a.documentId && frames.get("a/child").documentId !== ac.documentId);
    await reject("stale-main-document", () => send(a.tab.id, "invalid", {documentId:a.documentId}));
    await reject("stale-child-document", () => send(a.tab.id, "invalid", {documentId:ac.documentId}));
    reply = await send(a.tab.id, "new-document", {documentId:frames.get("a/main").documentId});
    check("new-document-reachable", reply.label === "a/main");
    await post("navigated");
    let command;
    do { await pause(); command = await (await fetch(CONTROL + "/command")).json(); } while (command.stage !== "closed");
    await reject("closed-tab", () => send(b.tab.id, "invalid", {}));
    await reject("no-listener", () => send(a.tab.id, "invalid", {}));
    await post("complete");
}
run().catch(error => post("failure", {fatal: String(error), stack: error.stack, frames: Object.fromEntries(frames)}));
