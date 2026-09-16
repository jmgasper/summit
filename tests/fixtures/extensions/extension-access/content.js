"use strict";
if (location.pathname.includes("/target/")) {
    const message = {kind: "extension-access-content", url: location.href, identity: browser.runtime.id,
        namespaces: Object.fromEntries([["browser", browser], ["chrome", chrome]].map(([name, api]) => [name, {
            present: typeof api.extension === "object", incognito: api.extension?.inIncognitoContext,
            urlMatches: typeof api.extension?.getURL === "function" && api.extension.getURL("view.html") === api.runtime.getURL("view.html"),
            restricted: Object.fromEntries(["getBackgroundPage", "getViews", "isAllowedFileSchemeAccess", "isAllowedIncognitoAccess"]
                .map(method => [method, typeof api.extension?.[method]]))
        }]))};
    browser.runtime.sendMessage(message).catch(error => console.error("ACCESS CONTENT ERROR", String(error)));
}
