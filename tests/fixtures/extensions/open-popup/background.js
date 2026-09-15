"use strict";
const action = browser.action || browser.browserAction;
let clicked = 0, lastCommand = 0;
action.onClicked.addListener(() => { ++clicked; action.setTitle({title: "Clicked " + clicked}); });
async function snapshot() {
    const tabs = await browser.tabs.query({active: true});
    return {clicked, tabs: tabs.map(tab => ({id: tab.id, windowId: tab.windowId,
        url: tab.url ?? null, title: tab.title ?? null, active: tab.active}))};
}
async function send(path, value) {
    const response = await fetch(POPUP_CONTROL + path, {method: "POST",
        headers: {"Content-Type": "application/json"}, body: JSON.stringify(value)});
    if (!response.ok) throw new Error("Control response " + response.status);
}
async function execute(command) {
    const api = command.namespace === "chrome" ? chrome : browser;
    const requestedAction = api.action || api.browserAction;
    let value;
    if (command.operation === "inspect") {
        value = {api: typeof requestedAction.openPopup, hostPermission: await api.permissions.contains({origins: ["http://localhost/*"]})};
    } else if (command.operation === "configure") {
        if (Object.hasOwn(command, "popup")) await requestedAction.setPopup({popup: command.popup});
        if (command.enabled === true) await requestedAction.enable();
        if (command.enabled === false) await requestedAction.disable();
    } else if (command.operation === "open") {
        if (Object.hasOwn(command, "popup"))
            await requestedAction.setPopup({popup: command.popup + "?request=" + command.id});
        const args = Object.hasOwn(command, "options") ? [command.options] : [];
        if (command.callback) {
            value = await new Promise((resolve, reject) => requestedAction.openPopup(...args, function() {
                const error = api.runtime.lastError;
                if (error) reject(new Error(error.message));
                else resolve({callbackArguments: arguments.length});
            }));
        } else {
            const result = await requestedAction.openPopup(...args);
            value = {resolvedUndefined: result === undefined};
        }
    } else throw new Error("Unknown command operation");
    return {id: command.id, ok: true, value, snapshot: await snapshot()};
}
(async () => {
    await action.setTitle({title: "OpenPopup ready"});
    await send("/ready", {ready: true});
    for (;;) {
        try {
            const response = await fetch(POPUP_CONTROL + "/command", {cache: "no-store"});
            const command = await response.json();
            if (Number.isInteger(command.id) && command.id > lastCommand) {
                lastCommand = command.id;
                // Keep accepting commands while an earlier openPopup is pending.
                execute(command).catch(async error => ({id: command.id, ok: false,
                    error: String(error), snapshot: await snapshot()}))
                    .then(result => send("/result/" + command.id, result))
                    .catch(error => console.error("POPUP CONTROL ERROR", String(error)));
            }
        } catch (error) { console.error("POPUP POLL ERROR", String(error)); }
        await new Promise(resolve => setTimeout(resolve, 40));
    }
})().catch(error => console.error("POPUP BACKGROUND ERROR", String(error)));
