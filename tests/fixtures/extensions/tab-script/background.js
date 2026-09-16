"use strict";
const cases = {};
const frames = [];
const check = (name, value) => { cases[name] = !!value; if (!value) throw new Error(name); };
const post = (phase, extra = {}) => fetch(CONTROL + "/report/" + phase, {
    method: "POST", headers: {"Content-Type": "application/json"},
    body: JSON.stringify({phase, runtimeId: browser.runtime.id, cases: {...cases}, ...extra})
});
const event = value => fetch(CONTROL + "/event", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(value)});
const pause = () => new Promise(resolve => setTimeout(resolve, 50));
const until = async predicate => {
    const deadline = Date.now() + 60000;
    while (!await predicate()) { if (Date.now() > deadline) throw new Error("Timed out waiting for injection fixture"); await pause(); }
};
const stage = name => until(async () => (await (await fetch(CONTROL + "/command")).json()).stage === name);
const reject = async (name, operation) => {
    let error;
    try { await operation(); } catch (value) { error = value; }
    check(name, !!error && typeof error.message === "string" && error.message.length > 0);
};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.kind === "script-sender") sendResponse({label: message.label, sender});
});
async function run() {
    await stage("initial");
    check("browser-api", typeof browser.tabs.executeScript === "function");
    check("chrome-api", typeof chrome.tabs.executeScript === "function");
    const tabs = await browser.tabs.query({});
    const a = tabs.find(tab => tab.url === CONTROL + "/target/a/main");
    const b = tabs.find(tab => tab.url === CONTROL + "/target/b/main");
    check("real-tabs", !!a && !!b && a.id !== b.id && b.active);
    const execute = (code, options = {}, tab = a.id) => browser.tabs.executeScript(tab, {code, ...options});
    const one = async (code, options = {}, tab = a.id) => {
        const result = await execute(code, options, tab);
        if (!Array.isArray(result) || result.length !== 1) throw new Error("Expected one script result");
        return result[0];
    };
    check("ordinary-completion", await one("20 + 22") === 42);
    check("last-statement", await one("1; 6 * 7;") === 42);
    check("empty-script", await one("") === undefined);
    check("undefined-result", await one("undefined") === undefined);
    check("null-result", await one("null") === null);
    check("false-result", await one("false") === false);
    check("zero-result", await one("0") === 0);
    check("unicode-result", await one('"雪"') === "雪");
    const label = "document.documentElement.dataset.label";
    check("omitted-current-tab", (await browser.tabs.executeScript({code: label}))[0] === "b/main");
    check("undefined-current-tab", (await browser.tabs.executeScript(undefined, {code: label}))[0] === "b/main");
    check("null-current-tab", (await browser.tabs.executeScript(null, {code: label}))[0] === "b/main");
    for (const [name, value] of [["zero",0], ["negative",-1], ["fraction",.5], ["nan",NaN], ["infinity",Infinity], ["unsafe",Number.MAX_SAFE_INTEGER + 1]])
        await reject("invalid-tab-" + name, () => browser.tabs.executeScript(value, {code: "true"}));
    await reject("missing-tab", () => browser.tabs.executeScript(999999999, {code: "true"}));
    check("page-world-hidden", await one("typeof pageOnly") === "undefined");
    check("content-runtime-id", await one("browser.runtime.id") === browser.runtime.id);
    check("content-privileged-tabs-hidden", await one('typeof browser.tabs') === "undefined");
    check("global-declaration", await one('var primaryOnly = "primary"; primaryOnly') === "primary");
    check("global-persists", await one("primaryOnly") === "primary");
    check("tab-global-isolation", await one("typeof primaryOnly", {}, b.id) === "undefined");
    await event({kind: "primary-ready", id: browser.runtime.id});
    const file = async path => (await browser.tabs.executeScript(a.id, {file: path}))[0];
    let result = await file("injected.js");
    check("packaged-file", result.count === 1 && result.id === browser.runtime.id && result.label === "a/main");
    result = await file("/injected.js");
    check("root-packaged-file", result.count === 2 && result.label === "a/main");
    await reject("missing-packaged-file", () => file("missing.js"));
    await reject("outside-packaged-file", () => file(CONTROL + "/target/a/main"));
    check("main-frame-only", JSON.stringify(await execute(label)) === '["a/main"]');
    let labels = await execute(label, {allFrames: true});
    check("all-frames-default", labels.length === 2 && labels[0] === "a/main" && labels[1] === "a/child");
    labels = await execute(label, {allFrames: true, matchAboutBlank: true});
    check("blank-and-srcdoc", labels.length === 4 && labels[0] === "a/main"
        && JSON.stringify([...labels].sort()) === '["a/blank","a/child","a/main","a/srcdoc"]');
    labels = await execute('if (document.documentElement.dataset.label === "a/child") throw new Error("child failure"); ' + label,
        {allFrames: true, matchAboutBlank: true});
    check("all-frames-child-error", labels.length === 3 && labels[0] === "a/main"
        && JSON.stringify([...labels].sort()) === '["a/blank","a/main","a/srcdoc"]');
    await reject("all-frames-main-error", () => execute('if (window === top) throw new Error("main failure"); ' + label,
        {allFrames: true, matchAboutBlank: true}));
    const senderCode = 'browser.runtime.sendMessage({kind:"script-sender", label:document.documentElement.dataset.label})';
    frames.push(...await execute(senderCode, {allFrames: true, matchAboutBlank: true}));
    const main = frames.find(frame => frame.label === "a/main");
    const child = frames.find(frame => frame.label === "a/child");
    const blank = frames.find(frame => frame.label === "a/blank");
    const srcdoc = frames.find(frame => frame.label === "a/srcdoc");
    const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
    check("injected-messaging", frames.length === 4 && frames.every(frame => frame.sender.id === browser.runtime.id && frame.sender.tab.id === a.id));
    check("frame-identifiers", main.sender.frameId === 0 && [child,blank,srcdoc].every(frame => frame.sender.frameId > 0)
        && new Set(frames.map(frame => frame.sender.frameId)).size === 4);
    check("document-identifiers", frames.every(frame => uuid.test(frame.sender.documentId)) && new Set(frames.map(frame => frame.sender.documentId)).size === 4);
    check("explicit-main-frame", await one(label, {frameId: 0}) === "a/main");
    check("explicit-child-frame", await one(label, {frameId: child.sender.frameId}) === "a/child");
    check("explicit-child-document", await one(label, {documentId: child.sender.documentId}) === "a/child");
    await reject("blank-without-match", () => execute(label, {frameId: blank.sender.frameId}));
    check("explicit-blank-frame", await one(label, {frameId: blank.sender.frameId, matchAboutBlank: true}) === "a/blank");
    check("explicit-srcdoc-document", await one(label, {documentId: srcdoc.sender.documentId, matchAboutBlank: true}) === "a/srcdoc");
    await reject("missing-frame", () => execute("true", {frameId: 999999999}));
    await reject("missing-document", () => execute("true", {documentId: "12345678-1234-4234-8234-123456789abc"}));
    await reject("frame-in-other-tab", () => execute("true", {frameId: child.sender.frameId}, b.id));
    await reject("document-in-other-tab", () => execute("true", {documentId: child.sender.documentId}, b.id));
    check("resolved-promise", await one("Promise.resolve(42)") === 42);
    check("delayed-promise", await one('new Promise(resolve => setTimeout(() => resolve("later"), 25))') === "later");
    await reject("rejected-promise", () => execute('Promise.reject(new Error("script rejection"))'));
    await reject("synchronous-exception", () => execute('throw new Error("script exception")'));
    await reject("syntax-error", () => execute("let = ;"));
    result = await one('({nested:[false,null,0,"雪"], value:42})');
    check("structured-object", JSON.stringify(result) === '{"nested":[false,null,0,"雪"],"value":42}');
    result = await one('new Map([["answer", 42]])');
    check("structured-map", result instanceof Map && result.size === 1 && result.get("answer") === 42);
    result = await one('new Set([42,"雪"])');
    check("structured-set", result instanceof Set && result.size === 2 && result.has(42) && result.has("雪"));
    check("structured-bigint", await one("12345678901234567890n") === 12345678901234567890n);
    result = await one("new Uint16Array([1,256,65535])");
    check("structured-typed-array", result instanceof Uint16Array && JSON.stringify([...result]) === "[1,256,65535]");
    result = await one("new Date(1234567890000)");
    check("structured-date", result instanceof Date && result.getTime() === 1234567890000);
    result = await one("/summit/gi");
    check("structured-regexp", result instanceof RegExp && result.source === "summit" && result.flags === "gi");
    result = await one("var cyclicResult = {value:42}; cyclicResult.self = cyclicResult; cyclicResult");
    check("structured-cycle", result.value === 42 && result.self === result);
    await reject("uncloneable-function", () => execute("(() => 42)"));
    await reject("uncloneable-symbol", () => execute('Symbol("result")'));
    await reject("uncloneable-dom", () => execute("document.body"));
    result = await new Promise((resolve, fail) => chrome.tabs.executeScript(a.id, {code: "42"}, value => {
        const error = chrome.runtime.lastError;
        error ? fail(new Error(error.message)) : resolve(value);
    }));
    check("chrome-callback", Array.isArray(result) && result.length === 1 && result[0] === 42);
    let lastError, callbackValue;
    await new Promise(resolve => chrome.tabs.executeScript(a.id, {code: "true", frameId: 999999999}, value => {
        lastError = chrome.runtime.lastError; callbackValue = value; resolve();
    }));
    check("callback-error-value", callbackValue === undefined);
    check("callback-last-error", !!lastError && typeof lastError.message === "string" && !chrome.runtime.lastError);
    for (const [name, details] of [
        ["missing-source", {}], ["both-sources", {code:"true",file:"injected.js"}],
        ["code-type", {code:42}], ["file-type", {file:42}], ["run-at", {code:"true",runAt:"later"}],
        ["all-frames-type", {code:"true",allFrames:"true"}], ["blank-type", {code:"true",matchAboutBlank:1}],
        ["frame-negative", {code:"true",frameId:-1}], ["frame-fraction", {code:"true",frameId:.5}],
        ["document-malformed", {code:"true",documentId:"bad"}], ["unsupported", {code:"true",unknown:true}],
        ["frame-and-document", {code:"true",frameId:0,documentId:main.sender.documentId}],
        ["all-and-frame", {code:"true",allFrames:true,frameId:0}],
        ["all-and-document", {code:"true",allFrames:true,documentId:main.sender.documentId}]
    ]) await reject("invalid-details-" + name, () => browser.tabs.executeScript(a.id, details));
    for (const runAt of ["document_start", "document_end", "document_idle"])
        check("loaded-" + runAt, await one("document.readyState", {runAt}) === "complete");
    await until(async () => {
        const state = await (await fetch(CONTROL + "/state")).json();
        const failed = state.events.find(event => event.kind.endsWith("-failure"));
        if (failed) throw new Error(failed.fatal);
        return state.events.some(event => event.kind === "other-ready") && state.events.some(event => event.kind === "denied-ready");
    });
    let state = await (await fetch(CONTROL + "/state")).json();
    const other = state.events.find(event => event.kind === "other-ready");
    const denied = state.events.find(event => event.kind === "denied-ready");
    check("other-extension-world", other.id === "summit-tab-script-other" && other.result.length === 1
        && other.result[0].primary === "undefined" && other.result[0].page === "undefined"
        && other.result[0].id === other.id && other.result[0].label === "a/main");
    check("primary-world-preserved", await one('primaryOnly === "primary" && typeof otherOnly === "undefined"'));
    check("tabs-grant-not-host-grant", denied.id === "summit-tab-script-denied" && denied.rejected && denied.metadata === a.url);
    check("denied-dom-untouched", await one('document.documentElement.dataset.denied === undefined'));
    await one('document.documentElement.dataset.injected = "primary"; true');
    await until(async () => (await (await fetch(CONTROL + "/state")).json()).observations.some(item => item.label === "a/main" && item.injected === "primary"));
    state = await (await fetch(CONTROL + "/state")).json();
    const observed = state.observations.filter(item => item.label === "a/main");
    check("page-observed-dom", observed.some(item => item.injected === "primary"));
    check("page-global-preserved", observed.every(item => item.pageOnly === "page" && item.primaryType === "undefined" && item.otherType === "undefined"));
    check("page-not-reloaded", new Set(observed.map(item => item.nonce)).size === 1 && state.loads["a/main"] === 1);
    check("child-dom-untouched", (await execute('document.documentElement.dataset.injected || ""', {allFrames: true, matchAboutBlank: true})).filter(Boolean).length === 1);
    let pendingSettled = false;
    const pending = execute('document.documentElement.dataset.pending = "waiting"; new Promise(() => {})').then(
        value => { pendingSettled = true; return {value}; },
        error => { pendingSettled = true; return {error}; });
    await until(async () => (await (await fetch(CONTROL + "/state")).json()).observations.some(item => item.label === "a/main" && item.pending === "waiting"));
    check("pending-script-started", await one('document.documentElement.dataset.pending') === "waiting");
    check("pending-result-waits", !pendingSettled);
    await post("initial", {frames});
    await stage("timing");
    const readiness = '({ready:document.readyState, parser:document.documentElement.dataset.parserReleased || ""})';
    let ended = false, idled = false, defaultIdled = false;
    const atEnd = one(readiness, {runAt: "document_end"}).then(value => { ended = true; return value; });
    const atIdle = one(readiness, {runAt: "document_idle"}).then(value => { idled = true; return value; });
    const atDefault = one(readiness).then(value => { defaultIdled = true; return value; });
    result = await one(readiness, {runAt: "document_start"});
    check("run-at-start-loading", result.ready === "loading" && result.parser === "");
    // Keep the parser blocked while the other asynchronous requests are delivered.
    await new Promise(resolve => setTimeout(resolve, 200));
    check("run-at-end-waits-parser", !ended);
    check("run-at-idle-waits-parser", !idled && !defaultIdled);
    const release = name => fetch(CONTROL + "/release/" + name, {method: "POST", headers: {"Content-Type": "application/json"}, body: "{}"});
    await release("parser");
    result = await atEnd;
    check("run-at-end-interactive", result.ready === "interactive" && result.parser === "true");
    await until(async () => (await (await fetch(CONTROL + "/state")).json()).holds.includes("load"));
    await new Promise(resolve => setTimeout(resolve, 200));
    check("run-at-idle-waits-load", !idled && !defaultIdled);
    check("run-at-start-after-parser", (await one(readiness, {runAt: "document_start"})).ready === "interactive");
    await release("load");
    result = await atIdle;
    check("run-at-idle-complete", result.ready === "complete" && result.parser === "true");
    result = await atDefault;
    check("run-at-default-idle", result.ready === "complete" && result.parser === "true");
    await post("timing");
    await stage("navigated");
    const cancelled = await pending;
    check("pending-navigation-cancelled", !!cancelled.error && typeof cancelled.error.message === "string" && cancelled.error.message.length > 0);
    await reject("stale-main-document", () => execute("true", {documentId: main.sender.documentId}));
    await reject("stale-child-document", () => execute("true", {documentId: child.sender.documentId}));
    await reject("stale-child-frame", () => execute("true", {frameId: child.sender.frameId}));
    check("replacement-world-clean", await one('typeof primaryOnly === "undefined" && typeof packagedCount === "undefined"'));
    const replacement = await one(senderCode);
    check("replacement-document", replacement.label === "a/main" && replacement.sender.tab.id === a.id && replacement.sender.documentId !== main.sender.documentId);
    check("replacement-injection", await one(label, {documentId: replacement.sender.documentId}) === "a/main");
    await post("navigated");
    await stage("complete");
    await reject("closed-tab", () => execute("true", {}, b.id));
    await reject("top-level-blank", () => execute("true"));
    await reject("top-level-blank-match", () => execute("true", {matchAboutBlank: true}));
    await post("complete");
}
run().catch(error => post("failure", {fatal: String(error), stack: error.stack, frames}));
