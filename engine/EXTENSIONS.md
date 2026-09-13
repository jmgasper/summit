# Extension runtime integration

Full Safari WebExtension, Chrome and Firefox compatibility remains an acceptance
requirement. There is no extension installer or execution runtime in Summit yet.
This design is grounded in the pinned September 13 WebKit source, rather than
an assumption that enabling a build option provides extension support.

## Reuse current WebKit where possible

Upstream has shared extension code under `Source/WebKit/UIProcess/Extensions`,
`Shared/Extensions` and `WebProcess/Extensions`. It includes manifest and match
pattern parsing, content worlds, API bindings, storage and messaging. GTK and
WPE enable `ENABLE_WK_WEB_EXTENSIONS` with their experimental feature switch.
Their platform adapters provide useful examples, but they do not establish
complete cross-browser API compatibility.

The intended runtime uses this shared implementation with native Haiku adapters.
Summit's tab, window, bookmark, download and permission models need adapters
to the corresponding extension APIs. Firefox-specific Promise semantics and
APIs, Chrome Manifest V2/V3 behavior, and Safari bundle import all require
explicit compatibility verification and additional implementation where absent.

## Prerequisite engine work

The current preview uses `WebKitLegacy`. Upstream's extension controller and
context proxy use the modern `WebKit` UI, web and network processes. A page's
ordinary JavaScript context is not an isolated extension execution environment.

The inherited `Source/WebKit/PlatformHaiku.cmake` references seven files absent
from the pinned upstream tree, including `AttachmentUnix.cpp`,
`SharedMemoryUnix.cpp`, and old drawing-area/compositing implementations.
Haiku's process initialization, network-process lifecycle, argument coders and
process-pool code contain unimplemented hooks. Turning on `ENABLE_WEBKIT` and
`ENABLE_WK_WEB_EXTENSIONS` cannot resolve these gaps.

The native process launcher now uses `posix_spawn` with an explicit IPC endpoint,
and resolves helper executables through Haiku's image API. Focused native tests
verify process creation, exact arguments, descriptor inheritance, sequenced
packets, descriptor transfer and shared mappings (18 checks), plus executable
discovery and failure cases (12 checks). These exercise the production spawn
helper and path resolver. The full modern process launcher and WebKit IPC
connection are still awaiting integration and compilation.

The shared-memory backend now owns transferable file descriptors, preserves
backing data beyond the allocating object's lifetime, and enforces read-only
handles. Thirty-nine native checks include descriptor transfer into a real
child, private mappings, and bitmap import by app_server. This establishes the
memory primitives; the complete WebKit IPC connection and process lifecycle
still need integration tests.

The next engine stages are:

1. Compile and exercise current JavaScriptCore, WebCore and native embedding.
2. Bring Haiku's modern process launcher, IPC descriptors, shared surfaces,
   process lifecycle, networking and storage onto current interfaces. Verify
   that killing a web process leaves the browser usable.
3. Connect a native `BView` to `WebPageProxy`, preserving Summit's native chrome
   while moving page execution into the web process.
4. Port extension platform resource, controller/context, window/tab, menu and
   permission adapters. Run upstream extension tests on those adapters.

## Browser integration and acceptance

Archive installation must validate CRX/XPI/ZIP or Safari resources, signatures
where applicable, resource boundaries, expanded size and manifest requirements.
Installation and updates should stage files atomically in the selected profile;
privilege changes require a visible permission decision. Runtime grants must
be enforced at API dispatch and content injection, not just at installation.

Compatibility tests must cover background pages and service workers, isolated
content scripts, frames and navigation timing, messaging/ports, actions and
popups, options, alarms, storage, cookies, request filtering, downloads and
native messaging. Adversarial tests must verify page/extension isolation,
cross-extension isolation, revoked grants, incognito separation, resource
traversal, and persistence across updates and restarts.

Passing an API subset is an intermediate result. Representative unmodified
extensions from all three ecosystems and the complete API coverage record
are required before making a compatibility claim. Native macOS Safari App
Extensions also contain platform binaries; importing their web resources alone
does not provide their native behavior.

Source references at the pinned commit:

- `Source/cmake/OptionsGTK.cmake`, `OptionsWPE.cmake`, `WebKitFeatures.cmake`
- `Source/WebKit/UIProcess/Extensions/WebExtension.cpp`, `WebExtensionMatchPattern.cpp`
- `Source/WebKit/UIProcess/Extensions/glib/WebExtensionContextGLib.cpp`
- `Source/WebKit/UIProcess/Extensions/WebExtensionTab.h`
- `Source/WebKit/PlatformHaiku.cmake`
