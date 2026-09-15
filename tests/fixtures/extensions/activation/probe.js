(async () => {
    const params = new URLSearchParams(location.hash.slice(1));
    const nonce = params.get("nonce");
    const round = Number(params.get("round"));
    const prefix = `SUMMIT ACTIVATION ${nonce} ${round} `;
    const output = document.getElementById("results");
    let count = 0;
    const check = (condition, message) => {
        if (!condition)
            throw new Error(message);
        ++count;
        output.textContent += `\nPASS ${message}`;
    };
    try {
        output.textContent = `Round ${round}`;
        check(browser.runtime.id === "summit-activation-fixture", "stable extension identity");
        check(browser.runtime.getManifest().name === "Summit activation fixture", "approved manifest snapshot");
        const granted = await browser.permissions.contains({
            permissions: ["storage", "cookies"], origins: ["https://summit-activation.invalid/*"]
        });
        check(granted === (round !== 3), "explicit or restored permission set");
        if (round === 3) {
            let rejected = false;
            try {
                await browser.cookies.getAll({url: "https://summit-activation.invalid/"});
            } catch {
                rejected = true;
            }
            check(rejected, "fresh empty approval blocks the privileged cookie operation");
        } else {
            let backgroundLoads = 0;
            for (let attempt = 0; attempt < 100 && backgroundLoads < round; ++attempt) {
                backgroundLoads = (await browser.storage.local.get("backgroundLoads")).backgroundLoads || 0;
                if (backgroundLoads < round)
                    await new Promise(resolve => setTimeout(resolve, 50));
            }
            check(backgroundLoads >= round, "background page executes after loading");
            const url = "https://summit-activation.invalid/";
            if (round === 1) {
                check((await browser.storage.local.get("activationNonce")).activationNonce === undefined,
                    "isolated profile starts without saved fixture data");
                await browser.storage.local.set({activationNonce: nonce});
                check((await browser.storage.local.get("activationNonce")).activationNonce === nonce,
                    "storage binding writes and reads through the loaded extension");
                await browser.cookies.set({url, name: "activation", value: nonce, secure: true});
            } else {
                check((await browser.storage.local.get("activationNonce")).activationNonce === nonce,
                    "extension storage survives unloading and startup restoration");
            }
            const cookie = await browser.cookies.get({url, name: "activation"});
            check(cookie && cookie.value === nonce, "granted cookie access reaches the browser store");
        }
        document.title = prefix + `PASS ${count}`;
    } catch (error) {
        output.textContent += `\nFAIL ${error.message}`;
        document.title = prefix + `FAIL ${error.message}`;
    }
})();
