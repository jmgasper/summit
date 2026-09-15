(async () => {
    const nonce = "@NONCE@";
    const old = await browser.storage.local.get(["bootCount", "nonce"]);
    if (old.nonce && old.nonce !== nonce)
        throw new Error("saved nonce belongs to another test");
    const boot = (old.bootCount || 0) + 1;
    await browser.storage.local.set({bootCount: boot, nonce});
    const granted = await browser.permissions.contains({
        permissions: ["storage"], origins: ["http://10.0.2.2/*"]
    });
    const report = {id: browser.runtime.id, boot, nonce, previousNonce: old.nonce || null, granted};
    const response = await fetch("@REPORT_URL@" + "&payload=" + encodeURIComponent(JSON.stringify(report)));
    if (!response.ok)
        throw new Error("fixture report rejected");
    await browser.storage.local.set({reportedBoot: boot});
})().catch(async error => {
    try { await browser.storage.local.set({startupError: String(error)}); } catch (_) { }
});
