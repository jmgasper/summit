(async () => {
    const nonce = location.hash.slice(1);
    const prefix = `SUMMIT STARTUP SEED ${nonce} `;
    try {
        for (let attempt = 0; attempt < 600; ++attempt) {
            const state = await browser.storage.local.get(["bootCount", "reportedBoot", "nonce", "startupError"]);
            if (state.startupError)
                throw new Error(state.startupError);
            if (state.bootCount === 1 && state.reportedBoot === 1 && state.nonce === nonce) {
                document.getElementById("result").textContent = "Approved background executed and saved its first boot.";
                document.title = prefix + "PASS 1";
                return;
            }
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        throw new Error("background report deadline");
    } catch (error) {
        document.getElementById("result").textContent = String(error);
        document.title = prefix + "FAIL " + String(error);
    }
})();
