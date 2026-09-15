"use strict";
if (/\/target\/[ab]\/(main|child)$/.test(location.pathname)) {
    const label = location.pathname.split("/target/")[1];
    const received = [];
    browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
        if (message === null || typeof message !== "object" || Array.isArray(message)) {
            sendResponse({value: message, label});
            return;
        }
        if (message.kind === "state") { sendResponse(received); return; }
        received.push(message.tag);
        const reply = {label, payload: message.payload, sender, id: browser.runtime.id};
        if (message.kind === "none") return;
        if (message.kind === "undefined") { sendResponse(); return; }
        if (message.kind === "value") { sendResponse(message.payload); return; }
        if (message.kind === "promise") return Promise.resolve(reply);
        if (message.kind === "later") { setTimeout(() => sendResponse(reply), 30); return true; }
        if (message.kind === "twice") { sendResponse("first"); sendResponse("second"); return; }
        sendResponse(reply);
    });
    browser.runtime.sendMessage({kind: "ready", label}).catch(error => {
        document.body.textContent = "Messaging handshake failed: " + error;
    });
}
