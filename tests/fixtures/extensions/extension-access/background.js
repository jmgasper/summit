"use strict";
globalThis.ACCESS_IDENTITY = browser.runtime.id;
globalThis.ACCESS_KIND = "background";
globalThis.ACCESS_BACKGROUND_OBJECT = {extension: browser.runtime.id, startup: crypto.randomUUID()};
const content = new Map();
let sequence = 0;
browser.runtime.onMessage.addListener((message, sender, reply) => {
    if (message?.kind !== "extension-access-content") return;
    content.set(message.url, {message, sender: {id: sender.id, url: sender.url, tabId: sender.tab?.id}});
    reply({identity: browser.runtime.id, startup: ACCESS_BACKGROUND_OBJECT.startup});
});
async function execute(command) {
    let value;
    if (command.operation === "inspect") {
        const background = browser.extension.getBackgroundPage();
        const rejects = {};
        for (const [label, filter] of [["array", []], ["boolean", false], ["type", {type: "bad"}],
            ["typeNumber", {type: 1}], ["tabFraction", {tabId: 1.5}], ["windowString", {windowId: "bad"}], ["unknown", {unknown: true}]]) {
            try { browser.extension.getViews(filter); rejects[label] = false; }
            catch (error) { rejects[label] = error instanceof Error && String(error).length > 0; }
        }
        value = {identity: browser.runtime.id, startup: ACCESS_BACKGROUND_OBJECT.startup,
            permissions: await accessPermissions(), incognito: browser.extension.inIncognitoContext,
            getURL: browser.extension.getURL("view.html"), expectedURL: browser.runtime.getURL("view.html"),
            normalizedURL: browser.extension.getURL("folder/../view.html"),
            selfBackground: background === globalThis, chromeBackground: chrome.extension.getBackgroundPage() === globalThis,
            all: accessViews(), popup: accessViews({type: "popup"}), tabs: accessViews({type: "tab"}),
            filtered: Object.hasOwn(command, "filter") ? accessViews(command.filter) : null,
            content: Array.from(content.values()), rejects, nativeTabs: await browser.tabs.query({})};
    } else if (command.operation === "open") {
        await browser.browserAction.setPopup({popup: "view.html?kind=popup&request=" + command.id});
        await browser.browserAction.openPopup();
        value = {opened: true};
    } else throw new Error("Unknown operation");
    return {id: command.id, ok: true, value};
}
(async () => {
    await accessSend("/ready", {identity: browser.runtime.id, startup: ACCESS_BACKGROUND_OBJECT.startup});
    for (;;) {
        try {
            const response = await fetch(ACCESS_CONTROL + "/command", {cache: "no-store"});
            const command = await response.json();
            if (Number.isInteger(command.id) && command.id > sequence) {
                sequence = command.id;
                const result = await execute(command).catch(error => ({id: command.id, ok: false, error: String(error)}));
                await accessSend("/result/" + command.id, result);
            }
        } catch (error) { console.error("ACCESS POLL ERROR", String(error)); }
        await new Promise(resolve => setTimeout(resolve, 40));
    }
})().catch(error => console.error("ACCESS BACKGROUND ERROR", String(error)));
