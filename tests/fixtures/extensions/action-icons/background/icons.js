// Each real toolbar click runs one case. The native harness observes pixels
// before issuing the next click, so coalesced updates cannot hide failures.
const action = browser.browserAction;
const NativeImageData = ImageData;
let firstTab, firstWindow, step = 0, running = false;
const image = (rgba = [80, 160, 224, 255], size = 16) => {
    const result = new NativeImageData(size, size);
    for (let i = 0; i < result.data.length; i += 4) result.data.set(rgba, i);
    return result;
};
const rejected = async (details, promiseRequired = false) => {
    let failed = false;
    if (promiseRequired) {
        const promise = action.setIcon(details);
        if (!promise || typeof promise.then !== "function") throw new Error("missing promise");
        try { await promise; } catch { failed = true; }
    } else {
        try { await action.setIcon(details); } catch { failed = true; }
    }
    if (!failed) throw new Error("invalid icon accepted");
};
const svg = (color, script = "") => "data:image/svg+xml," + encodeURIComponent(
    '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">' +
    '<rect width="16" height="16" fill="' + color + '"/>' + script + '</svg>');
const embeddedPNG = (rgba = [176, 96, 32, 255]) => {
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = 64;
    canvas.getContext("2d").putImageData(image(rgba, 64), 0, 0);
    return canvas.toDataURL("image/png");
};
const cases = [
    ["relative path", () => action.setIcon({path: "../icons/red.svg"})],
    ["rooted path", () => action.setIcon({path: "/icons/blue.svg"})],
    ["extension URL", () => action.setIcon({path: browser.runtime.getURL("icons/yellow.svg")})],
    ["size selection", () => action.setIcon({path: {32: "../icons/red.svg", 16: "../icons/blue.svg"}})],
    ["larger representation", () => action.setIcon({path: {32: "../icons/yellow.svg"}})],
    ["SVG data URL", () => action.setIcon({path: svg("rgb(144,48,176)")})],
    ["ImageData", () => action.setIcon({imageData: image()})],
    ["ImageData alpha", () => action.setIcon({imageData: image([80, 160, 224, 128])})],
    ["canvas alpha roundtrip", () => {
        // Exceed WebKit's 60x60 putImageData cache so readback exercises the
        // native bitmap backend, not the cache's simulated premultiplication.
        const canvas = document.createElement("canvas"); canvas.width = canvas.height = 64;
        const context = canvas.getContext("2d");
        context.putImageData(image([80, 160, 224, 128], 64), 0, 0);
        const clipped = context.getImageData(-1, -1, 66, 66);
        for (const [x, y, expected] of [[0, 0, [0, 0, 0, 0]], [1, 1, [80, 160, 224, 128]], [64, 64, [80, 160, 224, 128]], [65, 65, [0, 0, 0, 0]]]) {
            const pixel = Array.from(clipped.data.slice((y * 66 + x) * 4, (y * 66 + x) * 4 + 4));
            if (pixel.some((value, index) => value !== expected[index])) throw new Error("canvas clipped roundtrip " + pixel);
        }
        return action.setIcon({imageData: context.getImageData(0, 0, 16, 16)});
    }],
    ["canvas painted alpha", () => {
        const canvas = document.createElement("canvas"); canvas.width = canvas.height = 16;
        const context = canvas.getContext("2d");
        context.fillStyle = "rgba(80,160,224,0.5)"; context.fillRect(0, 0, 16, 16);
        const pixels = context.getImageData(0, 0, 16, 16);
        if ([80, 160, 224, 128].some((value, index) => Math.abs(pixels.data[index] - value) > 1)) throw new Error("canvas painted alpha " + Array.from(pixels.data.slice(0, 4)));
        return action.setIcon({imageData: pixels});
    }],
    ["ImageData size selection", () => action.setIcon({imageData: {32: image([208, 32, 48, 255], 32), 16: image()}})],
    ["ImageData snapshot", async () => {
        const pixels = image([176, 96, 32, 255]);
        const promise = action.setIcon({imageData: pixels});
        pixels.data.fill(255);
        await promise;
    }],
    ["native ImageData brand", async () => {
        const pixels = image();
        // WebKit exposes data as a nonconfigurable own property. Shadow the
        // configurable dimensions and typed-array properties instead.
        for (const key of ["buffer", "byteLength", "length"]) Object.defineProperty(pixels.data, key, {get() { throw new Error("shadow buffer getter"); }});
        for (const key of ["width", "height", "constructor"]) Object.defineProperty(pixels, key, {get() { throw new Error("shadow getter"); }});
        globalThis.ImageData = function () { throw new Error("shadow constructor"); };
        try { await action.setIcon({imageData: pixels}); } finally { globalThis.ImageData = NativeImageData; }
    }],
    ["inherited sizes", () => action.setIcon({path: Object.create({16: "../icons/red.svg"})})],
    ["inherited details", () => action.setIcon(Object.create({path: "../icons/blue.svg", get unknown() { throw new Error("unknown getter"); }}))],
    ["Chrome callback", () => new Promise((resolve, reject) => chrome.browserAction.setIcon({path: "../icons/yellow.svg"}, () => {
        const error = chrome.runtime.lastError;
        error ? reject(new Error(error.message)) : resolve();
    }))],
    ["null reset", () => action.setIcon({path: null})],
    ["empty dictionary reset", async () => { await action.setIcon({path: "../icons/red.svg"}); await action.setIcon({path: {}}); }],
    ["omitted reset", async () => { await action.setIcon({path: "../icons/blue.svg"}); await action.setIcon({}); }],
    ["null ImageData reset", async () => { await action.setIcon({imageData: image()}); await action.setIcon({imageData: null}); }],
    ["global icon", () => action.setIcon({path: "../icons/yellow.svg"})],
    ["missing resource rejection", () => rejected({path: "../icons/missing.png"}, true)],
    ["atomic size dictionary rejection", () => rejected({path: {16: "../icons/red.svg", 32: "../icons/missing.png"}}, true)],
    ["external resource rejection", () => rejected({path: "https://example.invalid/icon.png"})],
    ["malformed data rejection", async () => {
        for (const path of ["data:", "data:image/png;base64,%%%%", "data:image/png;base64,QUJDRA=="])
            await rejected({path}, true);
    }],
    ["ImageData lookalike rejection", () => rejected({imageData: {width: 16, height: 16, data: new Uint8ClampedArray(1024)}})],
    ["invalid size rejection", async () => {
        for (const key of ["0", "-1", "1.5", "016", "4294967296", "hello"])
            await rejected({path: {[key]: "../icons/red.svg"}});
    }],
    ["invalid details rejection", async () => {
        for (const details of [null, [], true, 1, "icon.svg", {path: ""}, {path: 4}, {imageData: []},
            {path: "../icons/red.svg", imageData: image()}, {path: "../icons/red.svg", tabId: null},
            {path: "../icons/red.svg", tabId: NaN}, {path: "../icons/red.svg", tabId: 9007199254740992}])
            await rejected(details);
    }],
    ["conflicting scope rejection", () => rejected({tabId: firstTab, windowId: firstWindow, path: "../icons/red.svg"})],
    ["throwing dictionary rejection", async () => {
        await rejected({get path() { throw new Error("details getter"); }});
        await rejected({path: {get 16() { throw new Error("size getter"); }}});
        await rejected({path: new Proxy({}, {ownKeys() { throw new Error("enumeration"); }})});
        const proxy = Proxy.revocable({}, {}); proxy.revoke();
        await rejected(proxy.proxy);
        await rejected({path: proxy.proxy});
    }],
    ["representation limit rejection", () => rejected({path: Object.fromEntries(Array.from({length: 257}, (_, i) => [i + 1, "../icons/red.svg"]))})],
    ["payload limit rejection", () => rejected({path: "data:image/png;base64," + "A".repeat(16 * 1024 * 1024)})],
    ["Chrome callback error", () => new Promise((resolve, reject) => chrome.browserAction.setIcon({path: "../icons/missing.png"}, () => {
        const error = chrome.runtime.lastError;
        error && error.message ? resolve() : reject(new Error("missing runtime.lastError"));
    }))],
    ["window override", () => action.setIcon({windowId: firstWindow, path: "../icons/red.svg"})],
    ["first tab override", () => action.setIcon({tabId: firstTab, path: "../icons/blue.svg"})],
    ["second tab override", tab => action.setIcon({tabId: tab.id, imageData: image()})],
    ["window reset", () => action.setIcon({windowId: firstWindow, path: null})],
    ["tab empty reset", tab => action.setIcon({tabId: tab.id, imageData: {}})],
    ["closed tab rejection", () => rejected({tabId: firstTab, path: "../icons/red.svg"}, true)],
    ["global reset", () => action.setIcon({imageData: undefined})],
    ["SVG alpha", () => action.setIcon({path: svg("rgba(80,160,224,0.5)")})],
    ["isolated SVG", () => action.setIcon({path: svg("rgb(144,48,176)", '<script>document.querySelector("rect").setAttribute("fill","black")</script>')})],
    ["SVG embedded PNG", () => action.setIcon({path: svg("rgb(144,48,176)", '<image width="16" height="16" href="' + embeddedPNG() + '"/>')})],
    ["SVG embedded PNG alpha", () => action.setIcon({path: svg("transparent", '<image width="16" height="16" href="' + embeddedPNG([80, 160, 224, 128]) + '"/>')})],
    ["SVG malformed embedded image", () => action.setIcon({path: svg("rgb(144,48,176)", '<image width="16" height="16" href="data:image/png;base64,%%%%"/>')})],
    ["detached ImageData rejection", async () => {
        const pixels = image();
        structuredClone(pixels.data.buffer, {transfer: [pixels.data.buffer]});
        await rejected({imageData: pixels});
    }],
    ["final reset", () => action.setIcon({path: undefined})],
];
action.onClicked.addListener(async tab => {
    if (running) return;
    running = true;
    try {
        firstTab ??= tab.id;
        firstWindow ??= tab.windowId;
        const [label, run] = cases[step];
        await run(tab);
        await action.setTitle({title: "Icon " + (++step) + ": " + label});
    } catch (error) {
        await action.setTitle({title: "ICON ERROR " + (step + 1) + ": " + String(error)});
    } finally { running = false; }
});
action.setTitle({title: "Icons ready"});
