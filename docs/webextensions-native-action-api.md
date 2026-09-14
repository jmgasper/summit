# Native action API state and dispatch

The Haiku port adds `getTitle`, `setTitle`, `getBadgeText`, `setBadgeText`,
`getPopup`, `setPopup`, `enable`, `disable`, `isEnabled`, and `onClicked` to
the native action bindings. The namespace retains the pinned manifest checks
for `action`, `browserAction`, and `pageAction`. **No extension has run these
APIs.** Patch
`4b8e914e8509911d780d93e72bae8ccff5d56dd97b51da470dd09ec711bae201`
is promoted. Its full extension-enabled engine build is pending.

Reads and writes cross the actual context IPC boundary. Receivers require a
loaded privileged context whose manifest declares an action. They independently
validate target identifiers and check that the requested tab/window is live and
accessible. Missing or inaccessible targets return an error. Getters use the
existing inherited action without creating an override; setters use the native
action model and its coalesced property-change observer.

Details accept either a tab or window scope, never both. IDs must be safe
positive integers, with the current-window sentinel accepted for window scope.
Setters require the corresponding `title`, `text`, or `popup` field. Null
resets inheritance and an empty string is an explicit override. Unknown fields,
invalid value types and embedded null characters are rejected. This parser is
stricter than the pinned Cocoa implementation for unknown fields. The title
getter preserves explicit empty labels. Popup state stores the pinned WebKit
path value; it does not normalize it into a qualified URL or display a popup.

`enable` and `disable` use raw JavaScript values because the binding generator's
JSON conversion accepts dictionaries, not scalar numbers. Their converter
accepts omitted/undefined values or valid numeric tab IDs without coercing
strings and booleans. The inherited optional-callback overload treats a single
null argument as an omitted callback/default target; an explicit null tab
argument supplied before a callback reaches the converter and is rejected.
The helper tests the converter directly, not that overload dispatcher.

The native click-event producer checks the current load, tab access and enabled
state, and suppresses events when a popup is configured. It wakes background
content and checks those conditions again before sending permission-filtered
tab parameters. The web-process dispatcher creates fresh protected values for
each listener within a WebCore user-gesture scope. The actual SDK/toolbar click
route and temporary `activeTab` host grants remain unfinished, so this event
producer is not yet called by a native toolbar action.

Native icon APIs, badge background colors and `openPopup` remain unavailable.
The Cocoa bindings retain those methods and adapt ordinary details to their
existing Objective-C implementations. Icon details reach Cocoa as raw
JavaScript values so its existing `ImageData` conversion is preserved. The
Xcode project registers the new argument helper in its bindings group and
header build phase. Cocoa compilation and runtime behavior have not been tested.

## Verification

The isolated native helper passes **125 checks**. It runs the actual structural
parser, the raw JavaScript tab-ID converter and the existing WebKit dictionary
converter against frozen JavaScriptCore/WTF and ICU. Coverage includes malformed
scopes, required fields, safe integer limits, non-finite numbers, null versus
empty values, Unicode and garbage collection. All four source units compile,
the executable runs from `BApplication::ReadyToRun`, source/library hashes stay
unchanged, and the native crash log records no events. WebCore and WebKit are
not linked into this helper.

**Seven native integration units compile**, including the API, privileged
receiver, generated bindings and regenerated IPC. **393 binding generation and
preprocessing checks pass** across the Cocoa/Haiku surfaces, including raw
numeric argument handling and the Cocoa icon call signature. Twenty of the
37 interfaces generate C++ bindings. An audit matches the current candidate
sources, headers and generated outputs to the tested inputs.

The first helper attempt compiled but failed to link because Haiku retained
unrelated exported WebCore-dependent functions. The final isolated helper uses
hidden symbol visibility and section collection, as existing helpers do; it
does not stub those functions or link a mismatched engine library.

These results do not execute action API IPC, model inheritance, observer
delivery, permission decisions, real click events, popup presentation or an
extension in the browser.

Evidence:

- `.vm/extension-action-api-validation.json`
- `.vm/extension-action-details-tests.amSqInFE/result.json`
- `.vm/extension-lifecycle-inputs.yawnB3OU/result.json`
- `.vm/extension-bindings-generated-37zlp8gp/binding-generation.json`
- `.vm/extension-binding-platforms-gtlumi_b/result.json`
