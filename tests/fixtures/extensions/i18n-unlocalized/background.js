"use strict";
(async () => {
    const report = {world: "unlocalized", cases: {}, exceptions: {}};
    const capture = (name, operation) => {
        try {
            const value = operation();
            report.cases[name] = value === undefined ? {undefined: true} : value;
        } catch (error) {
            report.cases[name] = {threw: true};
            report.exceptions[name] = String(error);
        }
    };
    try {
        report.runtimeId = browser.runtime.id;
        const api = browser.i18n;
        capture("type", () => typeof api.getMessage);
        capture("identity", () => api.getMessage("@@extension_id") === new URL(browser.runtime.getURL("/")).host);
        for (const key of ["@@ui_locale", "@@bidi_dir", "@@bidi_reversed_dir", "@@bidi_start_edge", "@@bidi_end_edge", "unknown", ""])
            capture(key, () => api.getMessage(key));
        capture("manifest_name", () => browser.runtime.getManifest().name);
        capture("ui_language", () => typeof api.getUILanguage() === "string" && api.getUILanguage().length > 0);
        capture("too_many_substitutions", () => api.getMessage("unknown", new Array(10)));
        capture("invalid_options", () => api.getMessage("unknown", [], {escapeLt: "true"}));
    } catch (error) { report.fatal = String(error); }
    try {
        const response = await fetch(I18N_CONTROL + "/report/unlocalized", {method: "POST",
            headers: {"Content-Type": "application/json"}, body: JSON.stringify(report)});
        if (!response.ok) throw new Error("Report response " + response.status);
    } catch (error) { console.error("UNLOCALIZED REPORT FAILED", String(error)); }
})();
