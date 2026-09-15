# Published Dark Reader integration

The native compatibility probe uses the unmodified Chrome MV2 release of Dark
Reader 4.9.131 from `compatibility/extensions.lock.json`. Its archive SHA-256 is
`c267d7663633fbe6981af46cb42e2176fb00a46e409789b82ea3cfe8e9afe311`.
All 88 archive entries are extracted without changing their contents. The runner
checks the source archive, extracted tree, installed package, staged harness and
frozen browser inputs. A passing API fixture is not evidence for this package.

```sh
python3 tools/test-modern-darkreader.py --bundle NATIVE_BUNDLE --watch
```

The harness installs through the native folder picker and consent UI. Its
controlled target page starts with known light colors and reports actual computed
colors, Dark Reader's dynamic-theme attributes and generated stylesheet classes.
The intended checks cover native popup presentation, page recoloring, fresh-page
behavior after disabling and reenabling through the native manager, and clean
shutdown. The extension receives no test script or rewritten configuration.
Screenshots preserve visible popup/page output. Popup control functionality and
browser restart persistence require additional checks.

The first runtime run against `bundle-omqykez8` **fails before consent**. The light
control page reports correctly, but the browser exits with a segment violation
while its `ExtensionPackages` worker snapshots `ui/assets/fonts`. Four recursive
`Walker::walk` frames each reserve about 64 KiB for a file-copy buffer, exhausting
the worker stack. This is a package-preparation failure, before Dark Reader's
background or content scripts execute. The owned process group drains, and all
recorded source/archive/bundle hashes remain unchanged. The native crash interval
contains the debugger event and is explicitly a failed interval.

Evidence:

- `.vm/modern-darkreader-753082efbff1659244f1111a/result.json`: first runtime failure.
- `.vm/modern-darkreader-753082efbff1659244f1111a/native-crash.report`: native worker stack and disassembly.
- `.vm/darkreader-stack-candidate/manifest.json`: baseline and candidate source fingerprints.

The fix moves the copy buffer into heap storage owned by the traversal,
shared across recursive calls. The public asynchronous package-preparation test
now includes a resource at the supported 32-component path limit, larger than one
copy buffer, and verifies the retained snapshot bytes. Engine patch
`2bff61f9155eb0a4d401cf456d516cc1f030c8e8a46686bb84d5030220d367ca`
builds successfully; the asynchronous test passes 65 checks with unchanged native
inputs and libraries, normal exit, drained processes and a clean crash interval.
Evidence: `.vm/content-rule-pipeline.ygicfIYh/result.json`.

The subsequent run against `bundle-lbm_guj3` installs Dark Reader successfully and
displays its native action and popup. **The compatibility probe still fails:** the
popup stays on "Loading, please wait" and the controlled page retains its original
light colors, without dynamic-theme attributes. The browser exits normally,
its process group drains, and its crash interval has no debugger events.
All archive, extracted and installed package bytes, staged sources and frozen
bundle hashes remain unchanged. Evidence:
`.vm/modern-darkreader-82638d1eb796352e4f39d0bd/result.json`.
This establishes the installation-crash fix, not a working Dark Reader runtime.

A temporary diagnostic build, `bundle-mu6e9qwk`, forwards console messages from
WebProcess when the runner's `--trace-console` option is used. Its run preserves
all package/bundle inputs and exits normally with no debugger events, while
retaining the failed page-theme assertion. Console output identifies unavailable
`tabs.sendMessage` and `tabs.create`, rejection of the `discarded` tab-query field,
and the missing `extension.isAllowedFileSchemeAccess` used while collecting popup
data. The published package also calls `tabs.executeScript` for reinjection into
existing tabs; that path requires verification after query support is added.

Diagnostic evidence is
`.vm/modern-darkreader-7199380f209f48a7f4fd5c32/result.json` and its `console.log`.
The temporary WebChromeClient trace is tracked in
`.vm/darkreader-console-baseline/manifest.json`. It has been removed from the
[tab-messaging implementation](webextensions-tab-messaging.md), which now passes
its full native build and 57-case messaging suite. These published extension
failures remain open compatibility requirements until this package passes.

With tab messaging available in `bundle-9lg9p4yz`, the unchanged package now
applies its dynamic dark theme after installation. Actual body colors change
from `rgb(255, 255, 255)` to `rgb(24, 26, 27)`, text and panel colors change, and
the document reports dynamic-theme attributes and nine Dark Reader stylesheets.
Disabling the extension restores the original colors on a fresh page. Reenabling
it still leaves the next page light, so the run fails after 63 passing native
checks. Its popup screenshot still shows "Loading, please wait". The browser
exits normally with drained processes, a clean crash interval and unchanged
archive, extracted/installed package, harness and bundle bytes. Evidence:
`.vm/modern-darkreader-08b345bc4bf5c11a9dc863b9/result.json` and its
`enabled.ppm`, `popup.ppm` and `failure.ppm` screenshots. Reenable behavior and
popup readiness remain separate unresolved runtime requirements.

A subsequent `discarded` query/metadata candidate is staged separately in
`.vm/tabs-discarded-candidate`. Four affected native source files compile with
unchanged configured inputs (`.vm/extension-lifecycle-inputs.f6SPmzoR/result.json`).
The regenerated extension IPC serializers also compile in isolation, including
the new field's encoders and decoders
(`.vm/extension-lifecycle-inputs.T4tdD2Vj/result.json`).
It is not yet promoted, linked or runtime-tested. The native browser currently
retains each open tab's page when another tab is selected, so the proposed
metadata reports `discarded: false` and uses that state to filter queries.
A native tab-discard operation and automatic reload on activation remain absent.

The separate `.vm/extension-access-candidate` ports the existing `extension`
interface to C++ bindings and adds native permission queries, extension-view
lookup, `getURL` and `inIncognitoContext`. File/private access queries read the
UI context's saved settings. The candidate passes 614 binding checks and six
native compile checks; an additional compile verifies native call-time guards.
Evidence is `.vm/extension-binding-platforms-havuz_58/result.json`,
`.vm/extension-lifecycle-inputs.2kfGSQ4L/result.json` and
`.vm/extension-lifecycle-inputs.sGOjYnCZ/result.json`. It is not promoted or
runtime-verified. Permission changes, view filtering, context restrictions and
Cocoa adapter compilation remain to be checked.
