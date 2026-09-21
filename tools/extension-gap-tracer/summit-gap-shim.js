// Diagnostic only: reports extension API gaps without changing a published package.
// tools/make-traced-extension.py injects this into a *copy* of an extension so that
// one run lists every missing namespace/method instead of stopping at the first.
(function () {
    "use strict";
    const log = (...parts) => console.warn("[SUMMIT-GAP]", ...parts);
    const describe = (value) => {
        try {
            return JSON.stringify(value, (key, item) => typeof item === "function" ? "<fn>" : item)?.slice(0, 400);
        } catch (error) {
            return "<unserializable>";
        }
    };
    const eventStub = (path) => ({
        addListener() { log("listen", path); },
        removeListener() { },
        hasListener() { return false; },
        hasListeners() { return false; },
    });
    const stub = (path) => new Proxy(function () { }, {
        get(target, property) {
            if (typeof property === "symbol" || property === "then" || property === "toJSON")
                return undefined;
            const child = path + "." + property;
            return /^on[A-Z]/.test(property) ? eventStub(child) : stub(child);
        },
        apply(target, self, parameters) {
            log("call", path, describe(parameters));
            const last = parameters.length ? parameters[parameters.length - 1] : null;
            if (typeof last === "function") {
                Promise.resolve().then(() => last());
                return undefined;
            }
            return Promise.resolve(undefined);
        },
    });
    const namespaces = [
        "action", "alarms", "bookmarks", "browserAction", "browsingData", "captivePortal", "clipboard",
        "commands", "contentSettings", "contextMenus", "contextualIdentities", "cookies",
        "declarativeNetRequest", "devtools", "dns", "dom", "downloads", "extension", "find", "history",
        "i18n", "identity", "idle", "management", "menus", "notifications", "offscreen", "pageAction",
        "permissions", "privacy", "proxy", "runtime", "scripting", "search", "sessions", "sidePanel",
        "sidebarAction", "storage", "tabs", "theme", "topSites", "userScripts", "webNavigation",
        "webRequest", "windows",
    ];
    const roots = new Set([globalThis.chrome, globalThis.browser].filter(Boolean));
    log("page", location.href, "chrome", typeof globalThis.chrome, "browser", typeof globalThis.browser,
        "same", globalThis.chrome === globalThis.browser);
    for (const root of roots) {
        const missing = [];
        for (const name of namespaces) {
            if (root[name] !== undefined)
                continue;
            missing.push(name);
            try {
                Object.defineProperty(root, name, { value: stub(name), configurable: true, writable: true });
            } catch (error) {
                log("unshimmable", name, String(error));
            }
        }
        log("missing-namespaces", missing.join(","));
        log("present-namespaces", namespaces.filter((name) => !missing.includes(name)).join(","));
    }
    // Members that real extensions commonly call inside namespaces that do exist.
    const members = [
        "browserAction.openPopup", "browserAction.setIcon", "browserAction.setPopup", "action.openPopup",
        "commands.getAll", "commands.update", "contextMenus.create", "contextMenus.removeAll", "contextMenus.update",
        "extension.getViews", "extension.getBackgroundPage", "permissions.contains", "permissions.request",
        "privacy.network", "privacy.services", "privacy.websites",
        "runtime.connectNative", "runtime.getBrowserInfo", "runtime.getPlatformInfo", "runtime.onMessageExternal",
        "runtime.onSuspend", "runtime.openOptionsPage", "runtime.reload", "runtime.requestUpdateCheck",
        "runtime.sendNativeMessage", "runtime.setUninstallURL",
        "storage.local", "storage.managed", "storage.session", "storage.sync", "storage.onChanged",
        "tabs.captureVisibleTab", "tabs.connect", "tabs.create", "tabs.duplicate", "tabs.executeScript",
        "tabs.get", "tabs.getCurrent", "tabs.goBack", "tabs.insertCSS", "tabs.move", "tabs.query", "tabs.reload",
        "tabs.remove", "tabs.removeCSS", "tabs.sendMessage", "tabs.update", "tabs.onActivated", "tabs.onCreated",
        "tabs.onRemoved", "tabs.onUpdated",
        "webNavigation.getAllFrames", "webNavigation.getFrame", "webNavigation.onCreatedNavigationTarget",
        "webRequest.handlerBehaviorChanged", "webRequest.onAuthRequired", "webRequest.onBeforeRequest",
        "windows.create", "windows.get", "windows.getAll", "windows.getCurrent", "windows.getLastFocused",
        "windows.remove", "windows.update", "windows.onCreated", "windows.onFocusChanged", "windows.onRemoved",
    ];
    for (const root of roots) {
        const missing = [];
        for (const path of members) {
            const [namespace, member] = path.split(".");
            const owner = root[namespace];
            if (!owner || typeof owner !== "object" || owner[member] !== undefined)
                continue;
            missing.push(path);
            try {
                Object.defineProperty(owner, member, { value: /^on[A-Z]/.test(member) ? eventStub(path) : stub(path), configurable: true, writable: true });
            } catch (error) {
                log("unshimmable", path, String(error));
            }
        }
        log("missing-members", missing.join(","));
    }
    // Extensions attach their own properties to API objects (1Password: a symbol-keyed logger).
    for (const root of roots) {
        if (!root.runtime)
            continue;
        const key = Symbol.for("summit.gap.probe");
        try {
            root.runtime[key] = 1;
            root.runtime.summitGapProbe = 2;
            log("expando", "symbol", root.runtime[key], "string", root.runtime.summitGapProbe,
                "sameObject", root.runtime === root.runtime);
        } catch (error) {
            log("expando-failed", String(error));
        }
    }
    // Log tab queries and script injection, whose arguments decide where scripts run.
    for (const root of roots) {
        const tabs = root.tabs;
        if (!tabs || tabs.__summitTraced)
            continue;
        for (const name of ["query", "executeScript", "insertCSS"]) {
            const original = tabs[name];
            if (typeof original !== "function")
                continue;
            try {
                tabs[name] = function (...parameters) {
                    log("tabs." + name, describe(parameters.filter((value) => typeof value !== "function")));
                    const result = original.apply(tabs, parameters);
                    if (result && typeof result.then === "function")
                        result.then((value) => log("tabs." + name + " ->", describe(value)), (error) => log("tabs." + name + " rejected", String(error)));
                    return result;
                };
            } catch (error) {
                log("tabs-unpatchable", name, String(error));
            }
        }
        tabs.__summitTraced = true;
    }
    // Report exceptions thrown by message listeners before an extension redacts them.
    for (const root of roots) {
        const event = root.runtime && root.runtime.onMessage;
        if (!event || event.__summitWrapped)
            continue;
        const add = event.addListener.bind(event);
        const wrappers = new Map();
        event.addListener = (listener) => {
            const wrapped = (message, sender, respond) => {
                try {
                    const result = listener(message, sender, respond);
                    if (result && typeof result.catch === "function")
                        result.catch((error) => log("listener-rejected", describe(message && message.name), String(error), error && error.stack));
                    return result;
                } catch (error) {
                    log("listener-threw", describe(message && message.name), String(error), error && error.stack,
                        "sender", describe(sender));
                    throw error;
                }
            };
            wrappers.set(listener, wrapped);
            return add(wrapped);
        };
        const remove = event.removeListener.bind(event);
        event.removeListener = (listener) => remove(wrappers.get(listener) || listener);
        event.__summitWrapped = true;
    }
    // Retry rejected listener registrations without their options so tracing can continue.
    for (const root of roots) {
        const webRequest = root.webRequest;
        if (!webRequest || typeof webRequest !== "object")
            continue;
        for (const name of ["onBeforeRequest", "onBeforeSendHeaders", "onSendHeaders", "onHeadersReceived",
            "onAuthRequired", "onBeforeRedirect", "onResponseStarted", "onCompleted", "onErrorOccurred"]) {
            const event = webRequest[name];
            if (!event || event.__summitTraced)
                continue;
            const original = event.addListener.bind(event);
            try {
                event.addListener = (listener, filter, extraInfoSpec) => {
                    try {
                        return original(listener, filter, extraInfoSpec);
                    } catch (error) {
                        log("webRequest-listener-rejected", name, describe(extraInfoSpec), String(error));
                        return original(listener, filter);
                    }
                };
                event.__summitTraced = true;
            } catch (error) {
                log("webRequest-unpatchable", name, String(error));
            }
        }
    }
    // Optional rendering diagnosis: SUMMIT_GAP_DUMP_TEXT names text whose element chain to describe.
    const dumpText = globalThis.__summitGapDumpText;
    if (dumpText && typeof document !== "undefined") {
        setTimeout(() => {
            // Match text, an image source or a CSS background image.
            const starts = [];
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
            while (walker.nextNode() && starts.length < 3) {
                if (walker.currentNode.nodeValue.includes(dumpText))
                    starts.push(walker.currentNode.parentElement);
            }
            for (const element of document.querySelectorAll("*")) {
                if (starts.length >= 6)
                    break;
                if ((element.currentSrc || element.src || "").includes(dumpText) || getComputedStyle(element).backgroundImage.includes(dumpText))
                    starts.push(element);
            }
            let count = 0;
            for (const start of starts) {
                ++count;
                log("dump-start", start.outerHTML.slice(0, 300));
                for (let element = start, depth = 0; element && depth < 8; element = element.parentElement, ++depth) {
                    const style = getComputedStyle(element);
                    log("dump", depth, element.tagName, element.className && String(element.className).slice(0, 60),
                        JSON.stringify({ bg: style.backgroundColor, bgImage: style.backgroundImage.slice(0, 120), color: style.color,
                            opacity: style.opacity, filter: style.filter, backdrop: style.backdropFilter || style.webkitBackdropFilter,
                            mask: style.maskImage || style.webkitMaskImage, clip: style.clipPath, shadow: style.boxShadow.slice(0, 80),
                            radius: style.borderRadius, overflow: style.overflow, transform: style.transform, blend: style.mixBlendMode,
                            willChange: style.willChange, position: style.position }));
                }
            }
            log("dump-done", count);
        }, 8000);
    }
    addEventListener("unhandledrejection", (event) => {
        const reason = event.reason;
        log("unhandledrejection", String(reason), reason && reason.stack ? String(reason.stack).slice(0, 1200) : "");
    });
    addEventListener("error", (event) => {
        log("error", event.message, event.filename + ":" + event.lineno + ":" + event.colno,
            event.error && event.error.stack ? String(event.error.stack).slice(0, 1200) : "");
    });
})();
