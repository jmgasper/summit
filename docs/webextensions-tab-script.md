# Native extension script injection

Manifest V2 `browser.tabs.executeScript` and `chrome.tabs.executeScript` now
execute source code or packaged files in isolated extension worlds inside
existing native browser tabs. No manifest content script or reload is needed.
Omitted, null and undefined tab IDs select the active tab of the current window.

The UI captures frame/document identities, requires existing host or activeTab
grants, and checks access again after document readiness. The `tabs` metadata
grant cannot authorize execution. File/private access remains separate. The
web process revalidates the document path, authorized URL, live extension
controller and isolated-world identity before evaluating ordinary script.
Global declarations persist; last-statement values and resolved promises return
through the existing structured-clone IPC transport.

Main-frame execution is the default. Frame/document targets, all frames and
embedded blank/srcdoc inheritance are supported. A failed requested root
rejects the call; failed children are omitted and the main result stays first.
Privileged pages and top-level blank documents are rejected.

## Native verification

The full engine and bundle `bundle-zwmpgvis` build with patch
`0ebce0e871f549817c218e0e8a92a4f441408201d46d3aedd2f60e66a6ace206`.
Six suites pass **2,026 native checks**:

| Suite | Native checks | Evidence under `.vm/` |
| --- | ---: | --- |
| Script injection | 523 | `modern-extension-tab-script-f7ad01f83e9b005e55db70d1/result.json` |
| Published Dark Reader | 78 | `modern-darkreader-2f787a410b4545772a98aa5d/result.json` |
| Tab messaging | 294 | `modern-extension-tab-messaging-0a9bbf017cd182d15e79b473/result.json` |
| Messaging lifecycle | 251 | `modern-extension-messaging-lifecycle-a6ae48897e753a268edc8e40/result.json` |
| Programmatic popups | 342 | `modern-extension-open-popup-fe9e7a13ad586eb8a5ce17ed/result.json` |
| Extension access | 538 | `modern-extension-extension-access-910f78013a3b4bc7ce786558/result.json` |

All browsers exit normally, groups drain, crash intervals are clean, and
source/package/bundle hashes match. Aggregate evidence is
`.vm/tabs-execute-script-runtime-validation.json`.

The injection suite has **113 unique JavaScript cases**, installed through the
real picker and consent UI after the target documents load. Its four reports
contain 95, 103, 110 and 113 cumulative cases. It verifies code/files, optional
and invalid IDs, callbacks/lastError, promises, exceptions, structured types,
isolated globals, page-observed DOM changes, independent extensions, and
rejection for a tabs-only extension. Frame/blank/srcdoc targeting, partial
all-frame errors, stale documents, closed tabs and pending-promise cancellation
are also exercised.

Controlled resources test actual loading boundaries: a blocked parser permits
start execution while loading; releasing it permits end execution while
interactive; a separately held image keeps explicit/default idle requests
pending until the document is complete.

The unmodified published Dark Reader 4.9.131 package themes a page that was open
before installation. Its native load-success sequence stays unchanged. Real
pointer input to popup Off/On restores the original colors and dark theme
without reloading. Other popup controls remain outside this coverage.

```sh
python3 tools/test-modern-extension-tab-script.py --bundle /path/to/native/bundle
SUMMIT_NATIVE_TMPDIR=/SummitExtensions/summit/tmp \
  python3 tools/test-modern-darkreader.py --bundle /path/to/native/bundle --watch
```

## Remaining compatibility work

Live host revocation, activeTab/file/private execution, unload/removal while
waiting, MV3 exposure and out-of-process iframe ancestors need additional
runtime coverage. The readiness waiter learns host revocation at the UI check
after readiness; it does not immediately cancel a never-ready document solely
on revocation. Remote ancestor IDs may be unavailable in a local frame tree
when site isolation is enabled.

Both namespaces currently await returned promises. Exact historical Chrome MV2
promise-result behavior remains compatibility work; its original execution
path differs from Firefox's awaited result. See the [Chromium MV2 implementation](https://raw.githubusercontent.com/chromium/chromium/130.0.6723.58/extensions/browser/api/execute_code_function.cc).
Structured-clone side data, other Dark Reader controls and restart persistence
also remain unverified. The full browser and Safari/Chrome/Firefox extension
compatibility requirements remain active.
