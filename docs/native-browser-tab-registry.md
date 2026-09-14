# Native browser tab registry and SDK bridge

Each native WebKit browsing context now owns a registry of browser windows,
ordered page IDs, active tabs and focused-window order. Updates validate the
entire window list before replacing its state. Duplicate pages and missing
active pages are rejected; a page move updates its previous ownership, and
closing the last page removes the window. Window identities are not recycled.
The registry stores metadata and does not retain WebKit pages or native windows.

`BWebKitContext::SetBrowserWindowTabs` accepts a window messenger and an ordered
list of live views from the owning window looper. Its queued application-thread
work resolves actual WebPageProxy IDs, checks that every view belongs to that
context and rejects the whole update when a view is closed or unavailable.
An optional asynchronous reply reports status, window identity, tab count and
active index. Page-close cleanup removes the page from the registry.

Summit submits its tab state after creation, selection and closure, on window
focus changes and before window teardown. These calls are limited to the modern
WebKit build; existing browsing contexts continue to own their separate stores
and process pools. There is no new visible browser flow.

Verified:

- `.vm/extension-browser-tab-registry-tests.wJ1xuWhD/result.json`: **543 native
  checks pass**, with normal exit, no crash-log events and unchanged inputs and
  libraries. These use real BMessengers targeting native handlers and generated
  page identifiers. They cover update validation, moves, focus, removal, snapshot
  independence, identity lifetime and every index of a 512-tab window. They do
  not instantiate WebPageProxy objects, browser windows or SDK objects.
- `.vm/extension-lifecycle-inputs.bEtonjcs/result.json`: **four engine units
  compile**, including the registry, browsing context, public SDK and page-close
  integration. Both extension flags are enabled in the isolated configuration.
- `.vm/browser-registry-app-native-compile-result.json`: **all five Summit units
  compile** against the staged SDK and browser changes; no linking or runtime.
- `.vm/extension-browser-registry-validation.json` verifies final source hashes
  against all three reports.

The native SDK, its reply delivery and browser integration have not been run.
In particular, the mixed-profile and closed-view SDK rejection paths have only
been compiled. This registry is not yet connected to WebExtensionController or
native WebExtensionTab/Window delegates. No extension has used it. Full engine,
browser and extension integration tests remain required.

Promoted patch: `0f24f49c6189020ec84b14732cd10bfa31b4c7f4a555ebaa224c18f9c63d5209`.
Probe baseline: `5358cf199868ababff67cf59212eb911603ae30f75b7980e3fae366f1b993945`.
