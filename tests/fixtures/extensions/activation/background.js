(async () => {
    const value = await browser.storage.local.get("backgroundLoads");
    await browser.storage.local.set({backgroundLoads: (value.backgroundLoads || 0) + 1});
})().catch(() => {
    // The final round deliberately approves no storage permission.
});
