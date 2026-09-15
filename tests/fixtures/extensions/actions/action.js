const actionReport = async (kind, data = {}) => {
    const payload = {id: browser.runtime.id, nonce: "@NONCE@", kind, ...data};
    const response = await fetch("@REPORT_URL@" + "&payload=" + encodeURIComponent(JSON.stringify(payload)));
    if (!response.ok) throw new Error("action report rejected");
};
let actionClicks = 0;
browser.browserAction.onClicked.addListener(async tab => {
    try {
        const click = ++actionClicks;
        if (click === 1) {
            await browser.browserAction.setTitle({tabId: tab.id, title: "Summit clicked tab"});
            await browser.browserAction.setBadgeText({tabId: tab.id, text: "1"});
            await browser.browserAction.setBadgeBackgroundColor({tabId: tab.id, color: "#c03040"});
            await browser.browserAction.setBadgeTextColor({tabId: tab.id, color: "white"});
            await browser.browserAction.disable(tab.id);
        } else {
            await browser.browserAction.setTitle({title: "Summit popup again"});
            await browser.browserAction.setPopup({popup: "popup.html"});
        }
        await actionReport("clicked", {click, tabId: tab.id, url: tab.url});
    } catch (error) { await actionReport("action-error", {error: String(error)}); }
});
(async () => {
    const colors = await verifyBadgeColors();
    await actionReport("badge-colors", colors);
    await browser.browserAction.setTitle({title: "Summit popup ready"});
    await browser.browserAction.setBadgeText({text: "R"});
    await browser.browserAction.setPopup({popup: "popup.html"});
    await browser.browserAction.enable();
})().catch(error => actionReport("action-error", {error: String(error)}));
