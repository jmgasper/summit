"use strict";
const match = location.pathname.match(/\/target\/(initial|reenabled|again|third|fourth)\/(main|child)$/);
if (match) {
    const [, phase, frame] = match;
    chrome.runtime.onMessage.addListener((message, sender, respond) => {
        if (message.kind === "echo") respond({phase, frame, route: message.route,
            startup: message.startup, id: browser.runtime.id, senderId: sender.id});
    });
    chrome.runtime.sendMessage({kind: "ready", phase, frame}, reply => {
        const error = chrome.runtime.lastError;
        fetch(CONTROL + "/report", {method: "POST", headers: {"Content-Type": "application/json"},
            body: JSON.stringify({kind: "handshake", phase, frame, runtimeId: browser.runtime.id,
                reply, error: error ? error.message : null})});
    });
}
