"use strict";
(async () => {
    const query = new URL(location.href).searchParams;
    globalThis.ACCESS_KIND = query.get("kind");
    globalThis.ACCESS_IDENTITY = browser.runtime.id;
    const background = browser.extension.getBackgroundPage();
    globalThis.ACCESS_BACKGROUND_OBJECT = background.ACCESS_BACKGROUND_OBJECT;
    background.ACCESS_LAST_VIEW = globalThis;
    document.querySelector("#status").textContent = "Real " + ACCESS_KIND + " for " + ACCESS_IDENTITY;
    await accessSend("/view/" + (query.get("request") || "tab"), {
        identity: browser.runtime.id, kind: ACCESS_KIND, url: location.href,
        permissions: await accessPermissions(), incognito: browser.extension.inIncognitoContext,
        backgroundIdentity: background.ACCESS_IDENTITY, startup: background.ACCESS_BACKGROUND_OBJECT.startup,
        realBackground: background.window === background && background.document.defaultView === background,
        roundTripIdentity: background.ACCESS_LAST_VIEW === globalThis,
        all: accessViews(), filtered: accessViews({type: ACCESS_KIND})
    });
})().catch(error => accessSend("/view/" + (new URL(location.href).searchParams.get("request") || "tab"), {error: String(error)}));
