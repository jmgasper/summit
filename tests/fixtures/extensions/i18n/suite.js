"use strict";
async function runI18nSuite(world) {
    const report = {world, cases: {}, exceptions: {}};
    const capture = (name, operation) => {
        if (Object.hasOwn(report.cases, name)) throw new Error("Duplicate case " + name);
        try {
            const value = operation();
            report.cases[name] = value === undefined ? {undefined: true} : value;
        } catch (error) {
            report.cases[name] = {threw: true};
            report.exceptions[name] = String(error);
        }
    };
    try {
        const api = browser.i18n;
        for (const method of ["getMessage", "getUILanguage", "getAcceptLanguages", "getPreferredSystemLanguages", "getSystemUILanguage"])
            capture("type_" + method, () => typeof api[method]);
        const message = (...args) => api.getMessage(...args);
        capture("hello", () => message("hello"));
        capture("case_insensitive", () => message("HeLLo"));
        capture("chrome_alias", () => chrome.i18n.getMessage("hello"));
        for (const key of ["empty", "unknown", ""])
            capture("empty_" + key, () => message(key));
        capture("required_name", () => message());
        for (const [name, value] of Object.entries({number: 42, null: null, undefined: undefined, object: {}, array: []}))
            capture("name_" + name, () => message(value));
        capture("extra_argument", () => message("hello", [], {}, 1));
        capture("scalar_string", () => message("one", "world"));
        capture("array_string", () => message("one", ["world"]));
        for (const [name, value] of Object.entries({number: 42, null: null, undefined: undefined, object: {}, boolean: true}))
            capture("scalar_" + name, () => message("one", value));
        capture("array_numbers", () => message("pair", [0, 42]));
        capture("array_null_boolean", () => message("pair", [null, true]));
        capture("array_objects", () => message("pair", [{}, [1, 2]]));
        capture("array_sparse", () => message("pair", new Array(2)));
        capture("array_too_long", () => message("one", new Array(10)));
        capture("array_throw_getter", () => message("one", Object.defineProperty([], 0, {get() { throw new Error("index getter"); }})));
        capture("array_throw_string", () => message("pair", [{toString() { throw new Error("string conversion"); }}, "tail"]));
        capture("array_proxy", () => message("one", new Proxy(["proxy"], {})));
        capture("named_adjacent", () => message("named", ["first", "second"]));
        capture("caller_dollars", () => message("one", "$$caller"));
        capture("caller_placeholder", () => message("repeated", ["$2", "second"]));
        capture("catalog_dollars", () => message("dollars", "value"));
        capture("unicode", () => message("unicode", "山"));
        capture("escape_absent", () => message("escape", "<user>"));
        capture("escape_true", () => message("escape", "<user>", {escapeLt: true}));
        capture("escape_false", () => message("escape", "<user>", {escapeLt: false}));
        capture("options_second_argument", () => message("escape", {escapeLt: true}));
        for (const [name, value] of Object.entries({null: null, undefined: undefined, empty: {}}))
            capture("options_" + name, () => message("escape", "<user>", value));
        for (const [name, value] of Object.entries({number: 1, string: "bad", array: [], function: () => {}}))
            capture("options_" + name, () => message("hello", [], value));
        capture("options_unknown", () => message("hello", [], {unexpected: true}));
        capture("options_hidden_unknown", () => message("hello", [], Object.defineProperty({}, "unexpected", {value: true})));
        capture("options_nonboolean", () => message("hello", [], {escapeLt: "true"}));
        capture("options_nullable_boolean", () => message("escape", "<user>", {escapeLt: null}));
        capture("options_throw_getter", () => message("hello", [], {get escapeLt() { throw new Error("option getter"); }}));
        capture("options_throw_keys", () => message("hello", [], new Proxy({}, {ownKeys() { throw new Error("option keys"); }})));
        capture("options_hidden_boolean", () => message("escape", "<user>", Object.defineProperty({}, "escapeLt", {value: true})));
        let getterCalls = 0;
        capture("options_getter", () => message("escape", "<user>", {get escapeLt() { ++getterCalls; return true; }}));
        capture("options_getter_once", () => getterCalls);
        let inheritedCalls = 0;
        capture("options_inherited", () => message("escape", "<user>", Object.create({get escapeLt() { ++inheritedCalls; return true; }})));
        capture("options_inherited_not_read", () => inheritedCalls);
        capture("options_symbol", () => message("hello", [], {[Symbol("ignored")]: true}));
        report.runtimeId = browser.runtime.id;
        report.locale = message("@@ui_locale");
        capture("extension_id", () => message("@@extension_id") === new URL(browser.runtime.getURL("/")).host);
        capture("manifest_name", () => browser.runtime.getManifest().name);
        capture("manifest_description", () => browser.runtime.getManifest().description);
        report.uiLanguage = api.getUILanguage();
        capture("ui_language", () => typeof report.uiLanguage === "string" && /^[a-z]{2,3}(?:-[a-z0-9]{2,8})*$/i.test(report.uiLanguage));
        const callbackResult = method => new Promise((resolve, reject) => {
            api[method](function(value) {
                if (browser.runtime.lastError) reject(new Error(browser.runtime.lastError.message));
                else resolve({value, argumentCount: arguments.length});
            });
        });
        for (const method of ["getAcceptLanguages", "getPreferredSystemLanguages", "getSystemUILanguage"]) {
            const pending = api[method]();
            capture("promise_" + method, () => pending instanceof Promise);
            const value = await pending;
            const callback = await callbackResult(method);
            capture("callback_" + method, () => callback.argumentCount === 1 && JSON.stringify(callback.value) === JSON.stringify(value));
            capture("value_" + method, () => method === "getSystemUILanguage" ? typeof value === "string" && value.length > 0
                : Array.isArray(value) && value.length > 0 && value.every(tag => typeof tag === "string" && tag.length > 0));
            report[method] = value;
        }
        const response = await fetch(I18N_CONTROL + "/accept", {cache: "no-store"});
        if (!response.ok) throw new Error("Accept-Language report failed");
        report.acceptHeader = (await response.json()).acceptLanguage;
        capture("accept_header", () => JSON.stringify(report.acceptHeader.split(",").map(value => value.trim().split(";")[0])) === JSON.stringify(report.getAcceptLanguages));
        const probe = document.getElementById("i18n-probe");
        // A missing direction rule must not pass through the page's default LTR.
        probe.style.direction = message("@@bidi_dir") === "ltr" ? "rtl" : "ltr";
        capture("localized_css", () => getComputedStyle(probe, "::after").content);
        capture("literal_css", () => getComputedStyle(probe, "::before").content);
        capture("css_color", () => getComputedStyle(probe).color);
        capture("css_direction", () => getComputedStyle(probe, "::after").direction === message("@@bidi_dir"));
        const cssSnapshot = () => ({
            readyState: document.readyState,
            connected: probe.isConnected,
            color: getComputedStyle(probe).color,
            before: getComputedStyle(probe, "::before").content,
            after: getComputedStyle(probe, "::after").content,
            direction: getComputedStyle(probe, "::after").direction
        });
        report.cssDiagnostics = {initial: cssSnapshot()};
        probe.textContent = "Localization checks completed";
    } catch (error) {
        report.fatal = String(error);
    }
    try {
        const response = await fetch(I18N_CONTROL + "/report/" + world, {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(report)});
        if (!response.ok) throw new Error("Report response " + response.status);
    } catch (error) { console.error("I18N REPORT FAILED", world, String(error)); }
}
