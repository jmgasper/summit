"use strict";
if (/\/target\/[ab]\/(main|child)$/.test(location.pathname)) {
    const label = location.pathname.split("/target/")[1];
    const report = value => fetch(CONTROL + "/isolation", {
        method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(value)
    });
    browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
        report({label, kind: "unexpected-message", message, sender});
        sendResponse({wrongExtension: browser.runtime.id});
    });
    report({label, kind: "ready", id: browser.runtime.id});
}
