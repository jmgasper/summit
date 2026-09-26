# Summit extension compatibility survey (2026-09-26)

Corpus: 32 packages (20 Chrome Web Store, 11 AMO, 1 real Safari Web Extension
build) plus a composed Safari variant of uBlock Origin Lite.

Status: runtime testing ran on X399 with `ext-survey-bundle` (a copy of
`bundle-q2h888fj`) and finished for 2 packages before X399 went down (~08:55
GMT). Every other verdict is static (package code + engine source). Patches:
all 12 apply with `git apply --check` individually and in sequence; the two
Summit patches pass `c++ -fsyntax-only` on the Haiku VM; none was built.

Result codes: W works, W- small gaps, P partly works, B broken.

## Runtime in the VM (27 September)

With X399 down, the whole corpus ran in the build VM (llvmpipe, 1280x800)
with the engine built from these sources (`tools/ext-survey/batch.sh` with
`SURVEY_ROOT=/SummitExtensions/summit/ext-survey`, `SURVEY_BUNDLE`,
`SURVEY_MESA`; lists `list1.txt` to `list4.txt`). All 32 packages now install
and start. What the runs found and what changed:

| Extension | Found | Now |
|---|---|---|
| uBlock Origin Lite | No redirect rule ever applied: WebCore applies redirects only where a page's policies list the extension's host access, and only Cocoa set them. Each redirect was also followed by "ignore following rules", so a refused redirect let the request through with no rule at all: AdSense and Google Analytics loaded. | Policies carry each extension's host patterns; redirects no longer stop later rules. A test page (scripts that report whether they ran) shows AdSense, Analytics, Tag Manager and GPT replaced by uBOL's surrogates and the Facebook pixel blocked. |
| uBlock Origin Lite | `storage.session` failed ("SQL logic error"): Haiku's SQLite has no URI support, so `file::memory:` was a file in the working directory shared by every extension and browser started there. | Opened as a URI (private, in memory). |
| Privacy Badger | Would not install (390 content-script hosts over a 256-entry limit); then its startup aborted on a `ws://*/*` webRequest pattern. | Covered patterns are left out, limits are 4096, WebSocket patterns are skipped. Initialization completes. |
| ClearURLs | `menus.create()` failed: it adds `Object.prototype.getOrDefault` and the parser rejected unknown properties. | Unknown menu properties are ignored (Safari); icon sizes come from own properties (Chrome). Cleans a Google redirect URL. |
| Multi-Account Containers | Ten commands whose descriptions name messages its locales lack were dropped; `commands.update()` failed. | Kept with their identifier as the description (Firefox). contextualIdentities remains unsupported. |
| Obsidian Web Clipper (Safari) | `_locales` without `default_locale`: every message was empty and `menus.create()` failed. | The best-matching supported locale is used. |
| uBlock Origin Lite on d3ward's tester | The web process crashed in the font cache (zero-size last-resort font equal to the cache's empty key). | Fixed; that page is now only an archive notice. |
| 15 of 32 runs | "Failed to create handle for shared memory buffer": Haiku's 256-descriptor limit, a frame buffer lost. | The process launcher raises it to 4096; gone from the reruns. |
| Grammarly | "Manage storage timeout" | Not a failure: its 3 s timer logs that even after `storage.managed.get()` answered (an API probe confirms both forms answer). |
| Tampermonkey | "tabs.onUpdated listener can only be registered during startup" | Its own guard, after `userScripts` is missing (not implemented). |

Password managers (no accounts in the VM, so install and start only): 1Password
8.12 (Firefox package) initializes, with only its expected desktop-app
connection failures; Bitwarden (Chrome) installs without file access and its
service worker starts quietly (it had spun on "Safari VaultTimeout"
exceptions, 878,706 in two minutes, under the Safari user agent); Bitwarden
(Firefox), Proton Pass and LastPass install and start without errors. Signing
in and autofill still need a real account on X399.

Still expected: Tree Style Tab (sidebar, `sessions`, `menus.onShown`),
Multi-Account Containers (`contextualIdentities`), Tampermonkey
(`userScripts`), Honey's WebGL in a worker. Earlier fixes from this run are
in the commit "Read back TextureMapper frames through the pixel buffer, and
fixes from a VM extension run" (Ghostery rule update race, Stylus host-access
DNR, Violentmonkey `setIcon`, OneTab `navigator.storage`, OffscreenCanvas).

| Extension | Package | MV, background | Result | Causes |
|---|---|---|---|---|
| Dark Reader 4.9.133 | CWS | MV3 SW | W (runtime): SW boots, page themed, popup, welcome tab via tabs.create | - |
| Bitwarden 2026.9.2 | CWS | MV3 SW | B (runtime): install fails; with file access, SW spins | RC4, RC5, RC7, RC3 |
| Bitwarden 2026.9.0 | AMO | MV2 page | B: same Safari-mode loop; install needs file access | RC4, RC5 |
| Proton Pass 1.41.1 | CWS | MV3 SW | P: popup/vault work; in-page autofill dead | RC3 |
| LastPass 4.156.0 | CWS | MV3 SW | P/B: popup works; autofill never starts | RC7, RC2, RC3 |
| uBO Lite 2026.920 | CWS | MV3 SW (module) | P: cosmetic + scriptlets only; no network blocking | RC1 |
| AdGuard 5.5.2 | CWS | MV3 SW | B: element hiding only | RC1, RC3, userScripts |
| Ghostery 10.6.4 | CWS | MV3 SW (module) | B: cosmetic + GPC only | RC1, RC3 |
| Privacy Badger 2026.9.15 | AMO | MV2 page | W-: cookie clobbering, widget re-enable fail | RC3 |
| ClearURLs 1.27.3 | AMO | MV2 | W | - |
| Decentraleyes 3.0.2 | AMO | MV3 background.scripts | W (MV3 event page unverified) | - |
| LocalCDN 2.6.86 | AMO | MV2 page | W-: Chromium feature set | - |
| Cookie AutoDelete 3.8.2 | AMO | MV2 | P, unsafe: deletes cookies of open tabs | RC10, browsingData |
| Violentmonkey 2.49.0 | AMO | MV2 | B: background throws at top level | RC6 |
| Tampermonkey 5.5.0 | CWS | MV3 SW | B: no userscript runs | userScripts, RC2 |
| Stylus 2.4.14 | CWS | MV3 SW | P: styles apply; CSP listener lost; sync broken | RC6, RC2, identity |
| Stylus 2.4.14 | AMO | MV2 page | W-: CSP listener lost | RC6 |
| Grammarly 14.1332 | CWS | MV3 SW | P: first start/OAuth unreliable | RC11, RC3 |
| Google Translate 2.0.17 | CWS | MV3 SW | P: page bubble and TTS dead | RC2 |
| Honey 19.6.0 | CWS | MV3 SW | W-: cashback activation fails silently | RC2, RC8 |
| Keepa 5.69 | CWS | MV3 SW (module) | W-: welcome page and crawl opt-in lost | RC6, RC2 |
| Momentum 2.27.7 | CWS | MV3 SW | P: never replaces new tab | RC9, RC2 |
| OneTab 2.21 | CWS | MV3 SW | W-: restoring a pinned tab aborts the restore | RC8 |
| Vimium 2.4.2 | CWS | MV3 SW (module) | P: install needs file access; hints/scroll work; vomnibar, zoom, tab moves fail | RC4, history/bookmarks, tabs.* |
| React DevTools 8.0 | CWS | MV3 SW | B: devtools_page never loads | devtools |
| JSON Formatter 0.10.2 | CWS | content scripts | W | - |
| Return YouTube Dislike 4.0.5 | CWS | MV3 SW | W | - |
| SponsorBlock 6.1.6 | CWS | MV3 SW | W | - |
| SponsorBlock 6.1.7 | AMO | MV2 event page | P?: inline page script vs YouTube CSP (unverified) | - |
| Tree Style Tab 4.4.7 | AMO | MV2 page | B: tabGroups.onCreated throws at top level; no sidebar | sidebar, tabGroups |
| Multi-Account Containers 8.3.8 | AMO | MV2 page | B, not graceful: contextualIdentities.onRemoved throws in init() | contextualIdentities |
| Obsidian Web Clipper 1.7.1 | Safari zip | MV3 SW | P: clip/copy/export work; obsidian:// has no handler | - |
| uBOL Safari (composed) | Safari variant | MV3 scripts+module, persistent:false | P: loads as event page; no network blocking | RC1 |

Bitwarden (Chrome) runtime evidence: `Exception Safari VaultTimeout` x878,706
in ~2 min, WebProcess ~1 core.

## Ranked root causes

Score = packages fixed x impact (3 unusable/harmful, 2 core feature, 1 secondary).

| # | Root cause | Score | Patch |
|---|---|---|---|
| RC1 | DNR translation all-or-nothing and narrow (`WebExtensionDeclarativeNetRequestRulesHaiku.cpp:90-139`) | 12 | 01 |
| RC2 | `offscreen` Cocoa-only (IDL `Platform=COCOA`, flag in `PlatformEnableCocoa.h`) | 12 | 02 (full rebuild) |
| RC3 | `executeScript({world:'MAIN'})` rejected (`WebExtensionContextAPIScriptingHaiku.cpp:324`) | 11 | 03 |
| RC4 | Install fails on `file:///*` matches (`src/ui/ExtensionInstaller.cpp:172`, `ExtensionPackageRegistryHaiku.cpp:330`) | 9 | 04 (Summit) |
| RC5 | Safari UA sends store builds down Safari-app paths (`UserAgentHaiku.cpp:63`) | 7 | 05 (risky) |
| RC6 | Chrome enum objects missing (runtime.OnInstalledReason..., webRequest.*Options) | 7 | 06 |
| - | userScripts absent (Tampermonkey) | 6 | design only |
| RC7 | MV3 `webRequestAuthProvider` unsupported (`WebExtensionAPIWebRequestEvent.cpp:100`) | 5 | 07 |
| RC8 | `pinned:true` rejected | 4 | 08 |
| RC9 | `chrome_url_overrides.newtab` unused | 3 | 09a + 09b (Summit) |
| RC10 | No `tab.cookieStoreId`; storeIds are `persistent-N` (Cookie AutoDelete data loss) | 3 | 10 |
| RC11 | `storage.managed` absent (Grammarly first start) | 2 | 11 |

Bitwarden needs 04, 05 and 07 (and 03 for passkeys in open tabs, 02 for
clipboard). Patch 05 risk: Ghostery then takes its Chrome paths and calls
`chrome.action.getUserSettings()` unguarded; re-verify 1Password (Firefox),
Dark Reader MV3 and uBO.

Not patched (sketches): userScripts (IDL, inline registered scripts, USER_SCRIPT
world, storage, per-extension toggle); sidebar (WK_WEB_EXTENSIONS_SIDEBAR is 0
upstream); contextualIdentities (one WebsiteDataStore per container);
history/bookmarks/sessions/search via profile.json; tabs.duplicate/zoom/move;
devtools; identity.launchWebAuthFlow; browsingData.

Safari: packages live in `.app/Contents/PlugIns/*.appex/Contents/Resources/`.
An importer needs to find the appex with `NSExtensionPointIdentifier`
`com.apple.Safari.web-extension`, load `Resources` unpacked, derive identity
from the appex `CFBundleIdentifier`, grant nativeMessaging but fail native calls
after >= 1 s (a fast failure in a retry loop becomes a busy loop), and keep
the Safari UA.

Tools (copied to `tools/ext-survey/`; packages and unpacked copies stay in `.cache/ext-survey/`): `fetch.sh`; `tools/inventory.py`, `idl_surface.py`, `gaps.py`;
`tools/extctl.cpp` (drives the Extensions window by team id),
`tools/run-ext.sh NAME PKG URL [files]`, `tools/batch.sh`;
`tools/trace-copy.py`. Runtime checks pending, in order: uBOL/AdGuard/Ghostery
with 01; Proton Pass with 03; Violentmonkey with 06; LastPass with 07;
Bitwarden with 04+05+07; Google Translate with 02; Tampermonkey; Momentum with
09; Cookie AutoDelete with 10; Multi-Account Containers and Tree Style Tab;
JSON Formatter inline JSON; SponsorBlock (Firefox) CSP; both Safari packages.
