"use strict";
async function run() {
    const deadline = Date.now() + 90000;
    while (!(await (await fetch(CONTROL + "/state")).json()).primaryReady) {
        if (Date.now() > deadline) throw new Error("Primary extension did not seed its world");
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    const tab = (await browser.tabs.query({})).find(tab => tab.url === CONTROL + "/target/a/main");
    if (!tab) throw new Error("Missing target tab");
    const result = await browser.tabs.executeScript(tab.id, {code:
        'var otherOnly = "other"; ({primary: typeof primaryOnly, page: typeof pageOnly, id: browser.runtime.id, label: document.documentElement.dataset.label})'});
    await fetch(CONTROL + "/event", {method: "POST", headers: {"Content-Type": "application/json"},
        body: JSON.stringify({kind: "other-ready", id: browser.runtime.id, result})});
}
run().catch(error => fetch(CONTROL + "/event", {method: "POST", headers: {"Content-Type": "application/json"},
    body: JSON.stringify({kind: "other-failure", id: browser.runtime.id, fatal: String(error)})}));
