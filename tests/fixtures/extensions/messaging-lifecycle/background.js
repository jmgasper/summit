"use strict";
const backgroundURL = browser.runtime.getURL("");
const startup = backgroundURL + Date.now() + ":" + Math.random();
const post = value => fetch(CONTROL + "/report", {
    method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(value)
});
chrome.runtime.onMessage.addListener((message, sender, respond) => {
    if (message.kind !== "ready") return;
    const {phase, frame} = message;
    (async () => {
        const routes = {};
        const options = {
            document: {documentId: sender.documentId},
            frame: {frameId: sender.frameId},
            combined: {documentId: sender.documentId, frameId: sender.frameId},
            callback: {documentId: sender.documentId}
        };
        for (const [route, target] of Object.entries(options)) {
            try {
                const request = {kind: "echo", phase, frame, route, startup};
                const reply = route === "callback" ? await new Promise((resolve, reject) => {
                    chrome.tabs.sendMessage(sender.tab.id, request, target, value => {
                        const error = chrome.runtime.lastError;
                        error ? reject(new Error(error.message)) : resolve(value);
                    });
                }) : await browser.tabs.sendMessage(sender.tab.id, request, target);
                routes[route] = reply.phase === phase && reply.frame === frame
                    && reply.route === route && reply.startup === startup
                    && reply.id === browser.runtime.id && reply.senderId === browser.runtime.id;
            } catch (error) {
                routes[route] = String(error);
            }
        }
        await post({kind: "routing", phase, frame, routes, startup, backgroundURL,
            runtimeId: browser.runtime.id, documentId: sender.documentId,
            frameId: sender.frameId, tabId: sender.tab.id, senderURL: sender.url});
        respond({startup, backgroundURL});
    })().catch(error => post({kind: "failure", phase, frame, error: String(error)}));
    return true;
});
