"use strict";
(async () => {
    const request = new URL(location.href).searchParams.get("request") || "trusted";
    const tabs = await browser.tabs.query({active: true});
    document.getElementById("status").textContent = "Extension API ready";
    const response = await fetch(POPUP_CONTROL + "/popup/" + request, {method: "POST",
        headers: {"Content-Type": "application/json"}, body: JSON.stringify({
            request, title: document.title, text: document.getElementById("status").textContent,
            extensionURL: browser.runtime.getURL("popup.html"),
            tabs: tabs.map(tab => ({id: tab.id, url: tab.url ?? null, title: tab.title ?? null}))})});
    if (!response.ok) throw new Error("Popup control response " + response.status);
})().catch(error => console.error("POPUP DOCUMENT ERROR", String(error)));
