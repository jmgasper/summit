const report = async (kind, data = {}) => {
    const payload = {id: browser.runtime.id, nonce: "@NONCE@", kind, ...data};
    const response = await fetch("@REPORT_URL@" + "&payload=" + encodeURIComponent(JSON.stringify(payload)));
    if (!response.ok) throw new Error("popup report rejected");
};
(async () => {
    const state = await browser.storage.local.get(["nonce", "popupCount"]);
    if (state.nonce !== "@NONCE@") throw new Error("popup did not share the approved extension storage");
    const count = (state.popupCount || 0) + 1;
    await browser.storage.local.set({popupCount: count});
    await browser.browserAction.setBadgeBackgroundColor({color: "rgb(80, 40, 120)"});
    await browser.browserAction.setBadgeTextColor({color: null});
    document.getElementById("status").textContent = "Popup " + count + " · saved state verified";
    await report("popup", {count, storedNonce: state.nonce});
    document.getElementById("switch").addEventListener("click", async () => {
        try {
            await browser.browserAction.setTitle({title: "Summit click ready"});
            await browser.browserAction.setBadgeText({text: "C"});
            await report("popup-button", {count});
            await browser.browserAction.setPopup({popup: ""});
        } catch (error) { await report("action-error", {error: String(error)}); }
    });
})().catch(error => report("action-error", {error: String(error)}));
