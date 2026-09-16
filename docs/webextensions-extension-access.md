# Extension access and view APIs

The native extension API candidate exposes `browser.extension` and
`chrome.extension`. File and private-browsing access queries use privileged IPC
and read the loaded context's saved permissions. They support promises and
callbacks; calling them does not grant access.

Manifest V2 extensions can use `extension.getURL()`. Extension pages can also
use `getBackgroundPage()` and `getViews()` to obtain their real JavaScript
`Window` objects. View filters select popup or tab views by type, tab identifier
and window identifier. Content scripts expose the resource URL and incognito
context properties, while the page-only APIs are hidden and guarded at call
time. The Cocoa implementations retain adapters for the shared C++ bindings;
a Cocoa build has not been verified here.

## Runtime verification

`tools/test-modern-extension-access.py --bundle <native-bundle>` installs two
independent Manifest V2 fixtures through the native picker and consent controls.
The suite checks:

- Chrome and browser aliases, promises and callbacks, initially denied access,
  different grants for each extension, changed grants and revocation across four
  browser launches.
- Actual background, popup and packaged-tab `Window` identities, shared live
  JavaScript objects, view filtering, closed-view removal and extension isolation.
- Resource URLs, invalid view filters, content-script API restrictions and real
  sender identity.
- Normal browser shutdown, drained process groups, native crash logs and
  unchanged installed packages, staged inputs and frozen bundle files.

The restart phases edit only the two access flags in the test profile's catalog
while the browser is stopped. This exercises loading saved settings; it does not
verify a post-install settings UI. Native private-window behavior, Manifest V3
API exposure and borrowed-method restrictions still require runtime coverage.

The full engine build passes. The September 16 native bundle `bundle-7v5zjcgj`
passes all five suites: extension access (538 checks across four launches),
published Dark Reader (66), lifecycle messaging (251), tab messaging (294,
including 77 JavaScript cases), and programmatic popups (342). Every owned
browser group drains normally, the monitored crash intervals are clean, and
the bundle, installed packages and test inputs remain unchanged. Evidence is
recorded in `.vm/extension-access-runtime-validation.json`.

Dark Reader's popup now renders its full controls; control interaction still
needs testing. The access suite also exposed a native-tab integration gap:
ordinary page configurations cannot load a privileged extension page. The
browser now resolves new extension tabs against the live load receipts and
uses `CreateExtensionView` on the application thread. The suite verifies the
real background/tab objects and rejects an unknown origin sharing a loaded
extension's hostname prefix. These dedicated views retain WebKit's restriction
to the owning extension's origin; general navigation between extension and web
pages in one tab still needs implementation.

## Related discarded-tab queries

The candidate also accepts the boolean `discarded` filter in `tabs.query()` and
returns `discarded: false` in native Tab objects. Summit currently retains every
open tab's page; hiding a tab does not discard it. The messaging fixture adds 20
cases for omitted, null, undefined, true, false and invalid filters, combined
filters, callbacks, and metadata carried through query, get and message-sender
results. Its complete 77-case JavaScript run passes. Optional null and undefined
dictionary fields are omitted by the binding conversion, matching Chromium and
Firefox. Implementing
`tabs.discard()` and native unloading/restoration remains separate work.
