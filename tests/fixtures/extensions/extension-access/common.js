"use strict";
async function accessSend(path, value) {
    const response = await fetch(ACCESS_CONTROL + path, {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(value)});
    if (!response.ok) throw new Error("Report failed: " + response.status);
}
async function accessPermissions() {
    const result = {};
    for (const [namespace, api] of [["browser", browser], ["chrome", chrome]]) {
        const values = {};
        for (const [name, method] of [["files", "isAllowedFileSchemeAccess"], ["private", "isAllowedIncognitoAccess"]]) {
            values[name + "Promise"] = await api.extension[method]();
            values[name + "Callback"] = await new Promise((resolve, reject) => api.extension[method](function(value) {
                if (api.runtime.lastError) reject(new Error(api.runtime.lastError.message));
                else if (arguments.length !== 1 || typeof value !== "boolean") reject(new Error("Invalid callback arguments"));
                else resolve(value);
            }));
        }
        result[namespace] = values;
    }
    return result;
}
function accessViews(filter) {
    const values = filter === undefined ? browser.extension.getViews() : browser.extension.getViews(filter);
    return {array: Array.isArray(values), unique: new Set(values).size === values.length,
        self: values.includes(globalThis), views: values.map(value => ({
            url: value.location.href, realWindow: value.window === value && value.document.defaultView === value,
            identity: value.ACCESS_IDENTITY ?? null,
            kind: value.ACCESS_KIND ?? null,
            sameBackgroundObject: value.ACCESS_BACKGROUND_OBJECT === browser.extension.getBackgroundPage().ACCESS_BACKGROUND_OBJECT
        }))};
}
