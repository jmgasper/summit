"use strict";
async function run() {
    const deadline = Date.now() + 90000;
    while (!(await (await fetch(CONTROL + "/state")).json()).primaryReady) {
        if (Date.now() > deadline) throw new Error("Primary extension did not start");
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    const tab = (await browser.tabs.query({})).find(tab => tab.url === CONTROL + "/target/a/main");
    if (!tab) throw new Error("Tabs permission did not expose target metadata");
    let error;
    try { await browser.tabs.executeScript(tab.id, {code: 'document.documentElement.dataset.denied = "injected"; true'}); }
    catch (value) { error = value; }
    await fetch(CONTROL + "/event", {method: "POST", headers: {"Content-Type": "application/json"},
        body: JSON.stringify({kind: "denied-ready", id: browser.runtime.id, metadata: tab.url,
            rejected: !!error && typeof error.message === "string" && error.message.length > 0})});
}
run().catch(error => fetch(CONTROL + "/event", {method: "POST", headers: {"Content-Type": "application/json"},
    body: JSON.stringify({kind: "denied-failure", id: browser.runtime.id, fatal: String(error)})}));
