# Native tab messaging

The Haiku `tabs.sendMessage` implementation passes the installed-extension
messaging suite. It
routes messages through privileged extension IPC to this extension's listeners in
the selected native tab. The renderer checks the actual page, frame and document
identifiers before dispatch. This follows the targeting contract in the
[Chrome tabs API](https://developer.chrome.com/docs/extensions/reference/api/tabs#method-sendMessage).

Delivery and response are separate IPC results. A matching listener may finish
without replying; the promise then resolves to `undefined`. A missing destination
rejects the promise or supplies `runtime.lastError` during a callback. The
distinction is described by [Mozilla's tabs.sendMessage documentation](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/tabs/sendMessage).
The aggregator waits for other receiving processes before concluding that there
was no response, while an explicit response wins immediately.

The candidate also allows JSON `null` as a message argument. Its binding modifier
preserves the required message argument and existing serialization checks. The
native runtime suite includes primitive, array, nested-object and null values,
plus rejection of a cyclic Chrome message and a function argument.

```sh
python3 tools/test-modern-extension-tab-messaging.py --bundle NATIVE_BUNDLE
```

The runner installs two fixtures through the actual folder picker and consent UI.
It opens two tabs, each with a child frame. The tested extension discovers real
sender identifiers through content-to-background messages and waits until the
second extension's listeners are ready. Expected cases cover reply styles,
tab/frame/document selection, invalid destinations, callback errors, extension
isolation, navigation and closure. There are 57 distinct expected JavaScript
cases; phase reports retain earlier results. All 57 pass, alongside 234 native
harness checks, on `bundle-9lg9p4yz`. The test also requires normal browser exit, drained owned
processes, complete clean crash-log coverage and unchanged recorded inputs.
Those lifecycle and integrity checks pass as well. Evidence:
`.vm/modern-extension-tab-messaging-426c0251fefad42e89cc7214/result.json`.
The same bundle passes the existing native popup regression (342 checks and 41
commands), with clean lifecycle and unchanged inputs:
`.vm/modern-extension-open-popup-079b7efbab2be9c103d419f5/result.json`.

The baseline run against `bundle-lbm_guj3` reaches the expected missing-method
failure after installing both fixtures and checking all four sender identities.
It exits normally with an empty crash interval and unchanged sources, packages
and bundle files. Evidence:
`.vm/modern-extension-tab-messaging-8f56bdd54b29dc74b0fdf8be/result.json`.
Its `extension_isolation_verified` field only records listener readiness and no
unexpected messages: no tab message was sent. The runner now requires the whole
runtime harness to pass before using that field as verified isolation evidence.

The reply-handling correction passes isolated native compilation of its browser,
renderer and regenerated IPC receiver sources, with unchanged configured inputs:
`.vm/extension-lifecycle-inputs.dIaas5Mz/result.json`. That check performed no
linking or runtime assertions. The promoted candidate, including null-message
support and removal of temporary console tracing, passes 564 binding checks:
`.vm/extension-binding-platforms-nlm4can0/result.json`.

The superseded `23c8b3ac…` full build was deliberately interrupted after the
receipt correction was ready. The replacement full build uses engine patch
`8ab1587be109339c220c6c9c1e8b1242bb85201583dde8264960dbb509e0269e`;
its state is `.vm/tabs-message-receipt-build-state.json`. Full native compilation
and linking passed with unchanged inputs. Its matching native bundle is
`/SummitExtensions/summit/build-modern-browser/bundle-xd1f_x2e`, recorded in
`.vm/modern-browser-tabs-message-bundle-result.json`.

The first candidate runtime run passes all 52 initial JavaScript cases and the
stale main-document check, then fails `stale-child-document`: an old child
document still accepts a message after navigation. The harness records 109
passing native checks and one failure, normal browser exit, drained processes,
unchanged inputs and an empty crash interval. Evidence:
`.vm/modern-extension-tab-messaging-422f51ca660eb5c8d90f40c0/result.json`.
The correction excludes inactive and cached documents
from native extension namespace enumeration while retaining registrations for
history restoration. It passes isolated native compilation with unchanged
configured inputs in `.vm/extension-lifecycle-inputs.KPHqlyNc/result.json` and is
promoted as engine patch
`e681321fb9cd8b729f491251d2ddd52d6db9a7a61ebfc8f48b5407dae95b7d68`.
Its full build and the unchanged 57-case runtime suite pass. The matching bundle
is `/SummitExtensions/summit/build-modern-browser/bundle-9lg9p4yz`, frozen in
`.vm/modern-browser-tabs-message-active-document-bundle-result.json`.

Before that runtime run, the VM boot filesystem stopped accepting writes during
optional bundle staging cleanup. The bundle build itself exited successfully;
the outer watcher failed while waiting for filesystem sync. The VM was saved to
a checkpoint and restarted. All 177 regular bundle files and nine external
build inputs had been flushed first. After remounting the build volume, 76
bundle/input/link checks and filesystem sync passed:
`.vm/tabs-message-post-recovery-verification.json` and
`.vm/tabs-message-vm-recovery.json`. The runtime run began only after recovery.

History restoration, private-tab access, pending-reply teardown,
listener errors and Firefox structured-clone semantics need further coverage.
Published Dark Reader and full Safari/Chrome/Firefox compatibility remain open.
