Native WebExtensions integration investigation, 2026-09-14

This records the native integration route for upstream WebKit
`00991b6cdc59937da6076c69a22ae6a9460a4445` and the implemented extension
resource adapters. Verification comprises **7 production translation units
compiled with extensions enabled, 27 native resource-path checks, and 111
native archive checks**. The archive helper and updated constructor also
have separate native compile evidence. Reports and exact scope are in the
[manifest/resource notes](../tools/test-engine-extension-manifest-core-notes.md)
and [ZIP/XPI notes](../tools/test-engine-extension-archives-notes.md).

The full-engine build keeps `ENABLE_WK_WEB_EXTENSIONS=OFF`; feature-on
compilation occurs only in isolated probes. The path and archive tests run
production helpers, not a manifest/controller/browser runtime. **No extension
is running in Summit.** The acceptance criteria in
[REQUIREMENTS.md](REQUIREMENTS.md) remain intact, including Safari, Chrome and
Firefox compatibility and adversarial isolation tests.

The useful starting point is WebKit's existing `WebExtension`,
`WebExtensionContext`, `WebExtensionController` and their WebProcess proxies.
The native embedding can attach these directly to `API::PageConfiguration`.
However, enabling `ENABLE_WK_WEB_EXTENSIONS` alone cannot produce a working
Haiku runtime. The pinned GTK/WPE implementation is incomplete, and many
apparently portable classes still require Cocoa implementations at link time.
Port the existing implementation across its platform boundaries; retain its
manifest parser, match patterns, content worlds, IPC and JS binding generator.

The older `WebProcess/InjectedBundle/API/glib/WebKitWebExtension` API is the
native WebProcess plug-in mechanism. It is distinct from the newer browser
WebExtensions classes under `UIProcess/API/glib`, despite the overlapping
name. Loading an injected bundle would not supply the requested extension
manifest, permissions or browser APIs.

All source paths below are relative to `.cache/WebKit`. Line references in
the implementation-route tables identify the pinned upstream baseline;
subsequent native edits may shift those line numbers.

| Area | Evidence and implication |
| --- | --- |
| Main feature gate | `Source/cmake/WebKitFeatures.cmake:325` defines `ENABLE_WK_WEB_EXTENSIONS`, private and off. `OptionsHaiku.cmake` does not override it. `Source/WTF/wtf/PlatformEnable.h:660` also defaults it off. Use a separate native build configuration with `-DENABLE_WEBKIT=ON -DENABLE_WK_WEB_EXTENSIONS=ON` when porting; the existing preview must not advertise extension support from this flag. |
| GTK/WPE status | `OptionsGTK.cmake:143` and `OptionsWPE.cmake:97` enable it with experimental features. GTK's old API explicitly disables it; `WebExtensionContextGLib.cpp` requires `ENABLE_2022_GLIB_API`. This is evidence of a developing port, not a complete runtime to wrap. |
| Native dependency choice | Do not enable `USE_GLIB` or `ENABLE_2022_GLIB_API` to make Haiku compile. Those branches introduce `GFile`, `GKeyFile`, GObject, GTK/WPE views and their settings/delegates. Native adapters can use WTF file/JSON helpers, Interface Kit objects and `WebPageProxy` instead. |
| Common sources | `Source/WebKit/Sources.txt:317`, `:609` and `:712` already list common extension storage, manifest, context/controller and proxy sources. `SourcesGTK.txt` and `SourcesWPE.txt` add the GLib resource/background adapters. New native adapters belong in `PlatformHaiku.cmake`; common extractions should remain in `Sources.txt`. |
| JS generation | `Source/WebKit/CMakeLists.txt:144` and `:178` split Objective-C and C++ IDLs; `GENERATE_IDL_BINDINGS` produces `JSWebExtensionAPIUnified.mm` and `.cpp`. Of 37 extension IDLs, only six have `UseCPPAPI`: alarms, namespace, runtime, test, web-page namespace and web-page runtime. `CodeGeneratorExtensions.pm:304` selects `.cpp` using that attribute. Most APIs still need conversion to its existing C++ path. |
| Namespace exposure | `WebExtensionAPINamespace.idl:36` onward marks almost every useful API, including `runtime`, `storage`, `tabs`, `scripting` and `webRequest`, `Platform=COCOA`. Matching methods in `WebExtensionAPINamespace.cpp` are also guarded. Moving an API to C++ requires changing the IDL, implementation and matching IPC gates together. |
| Context IPC | `WebExtensionController.cpp:56` registers both the normal and privileged context receivers only on Cocoa. `WebExtensionContext.messages.in` leaves alarms outside its Cocoa gates, with most other messages inside. Preserve both identifiers and each `[Validator=...]` when making these paths native. |
| Request filtering | `ENABLE_CONTENT_EXTENSIONS` is a separate CMake option, off by default and enabled by GTK/WPE. Haiku needs it for the existing content-rule engine. The declarative-net-request translator remains `_WKWebExtensionDeclarativeNetRequestTranslator.mm`; existing common SQLite/rule bookkeeping is not the translator or blocking WebRequest implementation. |
| Service workers | `Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml:5451` defaults `ServiceWorkersEnabled` to true for modern WebKit, false otherwise. This is a runtime preference in this checkout, not a missing `ENABLE_SERVICE_WORKER` CMake flag. Extension-scheme registration, worker JS bindings and lifecycle still need native integration. |
| Additional APIs | `WK_WEB_EXTENSIONS_ICON_VARIANTS`, `WK_WEB_EXTENSIONS_OFFSCREEN`, `WK_WEB_EXTENSIONS_SIDEBAR`, `WK_WEB_EXTENSIONS_BOOKMARKS`, `INSPECTOR_EXTENSIONS` and `DNR_ON_RULE_MATCHED_DEBUG` occur in source feature guards. They are not all independent CMake options here. Add deliberate port definitions and implementation as each becomes available; do not assume passing similarly named `-D` values exposes an API. |

The native embedding seam is already present in modern WebKit.
`UIProcess/API/APIPageConfiguration.h:161` exposes
`setWebExtensionController(RefPtr<WebExtensionController>&&)` and
`setWeakWebExtensionController(WebExtensionController*)`.
`WebPageProxy.cpp:968` retains them, `:1030` adds the page to the controller,
and `:14819` includes controller parameters in page creation. The WebProcess
consumes them only on Cocoa at `WebProcess/WebPage/WebPage.cpp:878`;
`WebPage.h:1868` currently returns a null controller proxy on Haiku. Those
gates must change together with the native proxy implementation.
Attach the controller before creating each Summit page, including new windows;
a JavaScript injection API in the application is not needed for this route.

Use one browser-owned controller configuration per actual profile, with its
chosen `WebsiteDataStore`, extension storage directory and durable extension
identifiers. `WebExtensionControllerConfiguration` already exposes
`setStorageDirectory`, `setDefaultWebsiteDataStore`, persistent/nonpersistent
factories, and a UUID constructor. The native adapter now implements path
creation and `copy` in
`UIProcess/Extensions/haiku/WebExtensionControllerConfigurationHaiku.cpp:37`.
It computes default paths through `StoragePathsHaiku`, creates real temporary
directories, and preserves the selected directory and data store when copied.
Its default path follows the application signature; the browser still needs
to supply each actual profile's directory and data store explicitly. Persistent
extension storage also requires a durable custom extension identifier:
`WebExtensionController.cpp:449` returns no directory without one.
Ordinary pages can retain the profile controller; controller-owned
background/popup pages should use the weak configuration reference to avoid
ownership cycles. Private
access remains a separate permission (`setHasAccessToPrivateData`) and data
store decision, with controller sets for private and nonprivate content
controllers. A second storage folder alone does not prove private isolation.

Extension documents also need a dedicated page-configuration path, separate
from ordinary tabs. `WebExtensionContextCocoa.mm:2580` applies MV2/MV3 CSP
mode, permission-derived CORS patterns, the required extension base URL,
process display name, cookie policy and appropriate background preferences.
Map these through the corresponding `API::PageConfiguration` and preferences
properties. Preserve the extension-origin restriction when implementing any
access relaxation. Conversely, ordinary pages need extension URL masking:
`APIPageConfiguration.cpp:228` currently defaults this only on Cocoa. Test
both sides of that boundary rather than applying extension-page privileges
to every page sharing a controller.

A thin native public facade should own these common C++ objects and dispatch
all runtime operations to the WebKit main loop. Interface Kit windows can
receive UI requests and report results through owned messages, following the
new `BWebKitView` bridge. The adapter needs actual tab/window identities and
events, permission prompts, actions/popups, menus, notifications, downloads,
bookmarks and native messaging. `WebExtensionTab` and `WebExtensionWindow`
currently use the Cocoa delegate protocols, and their implementations live in
`UIProcess/Extensions/Cocoa/WebExtensionTabCocoa.mm` and
`WebExtensionWindowCocoa.mm`. Introduce native delegate interfaces or extract
the shared logic behind C++ client interfaces; retain the existing extension
API semantics and identifiers.

The first manifest/resource compile gate is implemented by
`tools/test-engine-extension-manifest-core-in-vm.sh`. It reuses the actual
Haiku Modern compiler flags and generated headers, enabling extensions only
in a copied configuration. Its recorded seven-unit production pass covers
`UIProcess/Extensions/WebExtension.cpp`,
`UIProcess/Extensions/WebExtensionMatchPattern.cpp`,
`Shared/Extensions/WebExtensionLocalization.cpp`,
`Shared/Extensions/WebExtensionPermission.cpp`,
`Shared/Extensions/WebExtensionUtilities.cpp`,
`UIProcess/Extensions/WebExtensionResources.cpp` and
`UIProcess/Extensions/haiku/WebExtensionHaiku.cpp`. There is no dedicated
`SummitWebExtensionCoreCompile` CMake target.

`WebExtension.h` now limits the GFile constructor to GLib and provides a
native resource-path constructor for directories and ZIP/XPI files. The
existing `WebExtension(const JSON::Value&, Resources&&)` and
`WebExtension(Resources&&)` constructors retain the upstream in-memory
manifest path. Scheme registration moved unchanged into
`WebExtensionMatchPatternProcessPool.cpp`, keeping process-pool dependencies
out of the reusable matcher; that full-runtime unit now compiles in the lifecycle preflight described
below; linking still requires the genuine context/process implementations.

`WebExtensionResources.cpp` shares the unchanged GLib `resourceDataForPath`,
`recordError` and `bestIcon` bodies across non-Cocoa platforms. The native
adapter supplies real bitmap/SVG decoding through WebCore, native Icon
creation and screen-scale selection. The parser and matcher were retained;
no success stubs or duplicate implementations were introduced. Object
compilation does not establish manifest execution or a working extension.
The next gates are upstream manifest/match-pattern assertions linked against
compatible Modern dependencies, then the actual WebKit library and process
executables with the feature enabled and the remaining native runtime paths.
Keep those feature-on probes isolated from the full-engine build.


The lifecycle compile preflight is `tools/test-engine-extension-lifecycle-compile.py`.
It snapshots the five common controller/context/process-pool/proxy units and
extension headers, records hashes, and enables both extension features in a
copied native configuration without using the feature-disabled PCH. Its first
run failed all five units on the missing native view types. The adapted header
passes all five. The subsequent process-proxy extraction also compiles without
warnings. The Cocoa file relinquishes the moved definitions; the exhaustive
content-world switch now traps an invalid enum rather than falling through.
No objects in this probe are linked or executed.

Evidence (2026-09-14):

- Failed baseline: `.vm/extension-lifecycle-inputs.fscU4gSa/result.json`.
- Five-unit native type pass: `.vm/extension-lifecycle-inputs.mVSQdL15/result.json`.
- Final proxy extraction pass: `.vm/extension-lifecycle-inputs.jpZLPkp7/result.json`.
- Undefined-symbol inventory: `.vm/extension-lifecycle-native-types-undefined.json`.

`tools/build-webkit-in-vm.sh --modern-extensions all` now configures a separate
source/build tree at `/boot/home/summit-webkit-extensions`, with both
`ENABLE_WK_WEB_EXTENSIONS` and `ENABLE_CONTENT_EXTENSIONS` enabled. It uses the
same locked upstream and Haiku patch as the normal build, but shares no engine
objects or generated headers with it. This build path is an integration gate;
its existence does not establish a successful feature-enabled link or runtime.

**Feature-enabled controller runtime tests cannot use the feature-disabled
WebKit library.** `ENABLE_WK_WEB_EXTENSIONS` changes object layouts and the
generated IPC contract:

| Current source | Feature-enabled difference |
| --- | --- |
| `UIProcess/API/APIPageConfiguration.h:535` | Adds the required extension URL and strong/weak controller references to configuration data. |
| `UIProcess/WebPageProxy.h:3876` | Adds strong/weak controller references to each page proxy. |
| `Shared/WebPageCreationParameters.h:284` | Adds controller parameters to the page-creation structure and its serialized payload. |
| `Shared/API/APIObject.h:190` | Inserts extension API type IDs before existing types, changing subsequent values including `WebsiteDataStore` and `WebsitePolicies`. |

A successful link does not establish compatibility. Controller and
configuration runtime tests need a matching feature-enabled WebKit dependency
closure, including the generated headers/serializers and any process
executables they use. Keep these probes compile-only until that closure is
available; linking selected feature-enabled objects to the baseline
feature-disabled `libWebKit.so` is unsafe. The standalone native path/archive
helper checks do not cross this boundary and do not demonstrate controller
execution.

The next proposed native slice is profile-owned controller/context lifecycle
and resource serving. `UIProcess/haiku/WebViewContextHaiku` is the existing
owner of the profile's process pool and data store; it should also own the
controller and attach it in `configure(API::PageConfiguration&)`. Preserve
the common controller load/unload implementation at
`WebExtensionController.cpp:147`, including duplicate-context/base-URL
rejection, rollback and process notifications. Proposed implementation files
under `UIProcess/Extensions/haiku` are:

| Proposed file; not implemented | Required scope |
| --- | --- |
| `WebExtensionControllerHaiku.cpp` | Native platform/client initialization. Port the five-second freshly-created expiry at `WebExtensionController.cpp:101`; generated controller message dispatch also needs the seven test callbacks currently in `Cocoa/API/WebExtensionControllerAPITestCocoa.mm:42`. |
| `WebExtensionContextHaiku.cpp` | Native extension construction, state/errors, load/unload/reload, storage invalidation and page identities. Preserve the sequencing and cleanup in `Cocoa/WebExtensionContextCocoa.mm:278`, including asynchronous unloaded-context checks and notifying WebProcesses before injection. |
| `WebExtensionURLSchemeHandlerHaiku.cpp` | Constructor plus start/stop/completed task methods, preserving the resource permission, response and cancellation semantics at `Cocoa/WebExtensionURLSchemeHandlerCocoa.mm:58`. |

The header now declares the native extension constructor and maps native
view/navigation types to `WebPageProxy`, `API::PageConfiguration` and
`API::NavigationAction`. Native state uses a JSON object and `State.json`;
background ownership uses `RefPtr<WebPageProxy>`. GLib state declarations
are limited to their feature guard. These declarations do not yet implement
native state persistence, background construction or context load/unload. `CocoaMenuItem`
declarations and the `NSBlockOperation` map at
`WebExtensionURLSchemeHandler.h:56` also require platform separation.
Adding only an empty controller platform hook cannot supply this runtime.
The first lifecycle fixture against matching dependencies can use a manifest
without scripts or background content to verify registration, duplicate rejection, rollback,
unload/reset and profile persistence. That establishes lifecycle behavior;
content-script/background execution still requires the proxy, binding and
page integration gates below.

| Required port work after the manifest-core compile | Existing implementation to preserve |
| --- | --- |
| Context construction, load/unload and state | `WebExtensionContextCocoa.mm:278` performs controller attachment, content-world creation, persisted state, install reason, tab population, data migration, registered scripts, rules and load notification. GLib implements state-file helpers and background views, but does not implement the complete load/unload path. Extract common sequencing and replace only persisted-value and native view/delegate access. |
| Required load ordering | The Cocoa `load` path calls `controller->dispatchDidLoad(*this)` before `addInjectedContent()`, so the WebProcess knows the extension world before a content script runs. Preserve this order and unloaded-context checks in asynchronous callbacks. |
| Resource scheme | `WebExtensionURLSchemeHandler.h` derives from the existing `WebURLSchemeHandler`; its only implementation is `Cocoa/WebExtensionURLSchemeHandlerCocoa.mm`. Replace `NSBlockOperation`, NSError and NSHTTPURLResponse with cancellable main-loop tasks and WebCore responses/errors. Keep the controller lookup, same-origin/`web_accessible_resources` checks, required extension base URL, CSP, MIME type, localization, task cancellation and indistinguishable permission failures. |
| Scheme registration | `WebExtensionMatchPattern.cpp` defaults to `webkit-extension`; the unchanged `registerCustomURLScheme` body now lives in `WebExtensionMatchPatternProcessPool.cpp` and updates match-pattern sets, service-worker-client registration and existing WebProcesses. Use the existing base URL mechanism. Chrome/Firefox URL identity expectations require explicit compatibility tests before adopting additional schemes. |
| Background documents/workers | GLib's `loadBackgroundWebView` configures MV2/MV3 preferences, foreground process activity, load/failure/process-exit handlers and either URL loading or worker loading. Native hidden `WebPageProxy` instances should follow the same lifecycle. `WebPageProxy::loadServiceWorker`, implemented in common `WebPageProxy.cpp:18701`, is the worker entry point. Retain persistent/event-background behavior, listeners, wake-up tasks and unload timers. |
| WebProcess context lifecycle | Constructors, `getOrCreate`, state updates, permission expiry, localization parsing and receiver registration now live in common `WebExtensionContextProxy.cpp`. The extraction preserves privileged identifier distribution and frame/page tracking. Script-error IPC remains in the Cocoa file until the native UI receiver exists. |
| JS world binding | `WebExtensionControllerProxyCocoa.mm:54` installs `browser` and `chrome` onto the correct normal or isolated world using the generated wrapper. Much of this file is already C++ and can move to a common source. `WebLocalFrameLoaderClient.cpp:1965`, `:1979` and `:1993` still invoke the hooks only for Cocoa, including the service-worker and pre-user-script hooks. Broaden those gates only together with the working native implementation. |
| Content scripts and permission changes | Common `WebExtensionContext.cpp:1221` onward converts injected content to `API::UserScript`/`API::UserStyleSheet`, computes granted patterns and installs them through `WebUserContentControllerProxy`. `toContentWorld` chooses the extension's isolated world. Permission-change broadcasting and several removal/revocation side effects still live in Cocoa. Preserve those transitions as well as initial grants. |
| Core API messages | Convert existing event, runtime/port and storage bindings through `UseCPPAPI`, with `JSONValue`/WTF types and existing callbacks. `JSWebExtensionWrapper.cpp` exists but is not in common `Sources.txt`; include it when native bindings are linked. Move generated interfaces between CMake lists when their IDL output changes. The namespace exposure, API methods, context messages and message validators must agree. |
| Native UI APIs | Port action, tabs/windows, commands, menus, permissions, notifications and associated events against Summit's real browser objects. Returning success for an unimplemented delegate would conceal an extension compatibility failure. |
| Network APIs | Reuse common content-rule and SQLite stores, port the existing DNR translator's semantics, and connect the controller's request event hooks in `WebPageProxy.cpp:12504` onward. The Cocoa controller and API handlers remain necessary source references. Verify redirects, headers, cancellation and private sessions through the Curl NetworkProcess; notification hooks alone do not establish blocking WebRequest. |

Native ZIP/XPI resource loading is implemented in
`UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.cpp` using libzip 1.11.4
in the recorded tests. `PlatformHaiku.cmake` requires `libzip>=1.11` only
when extensions are enabled. The native resource constructor preserves
directory loading, extracts stored/deflated ZIP or ZIP64 into a newly owned
private directory, and transfers cleanup ownership only after upstream
manifest parsing succeeds. The helper's **111 native checks** cover valid
packages, resource preservation, traversal, links/special entries,
corruption/encryption, expansion bounds and temporary-directory cleanup.
Those tests do not instantiate WebExtension or run its manifest parser.

The native loader does not call the upstream CRX-prefix conversion or the
non-Cocoa `FileSystem::extractTemporaryZipArchive` stub. CRX/self-extracting
prefixes and non-ZIP formats are rejected. Archive extraction is distinct
from a complete installation/update flow: package identity and signature
verification, trust decisions, permissions, durable installation, rollback
and update migration remain to be implemented and tested.

Native resource containment now uses explicit canonicalization failure,
separator-aware ancestry and correctly escaped file URLs. Its **27 native
checks** cover sibling-prefix and symlink escapes, encoded/raw NUL, traversal,
Unicode and percent filenames. The common non-Haiku method also uses
`FileSystem::isAncestor` in place of the original textual-prefix check.
Archive extraction uses descriptor-relative operations; the resource-path
tests establish static canonical containment, not atomic protection against
concurrent filesystem replacement between checking and opening a resource.

Safari WebExtension resources can enter the same upstream manifest/runtime
after locating their bundle resource directory and preserving their identity
and metadata. Cocoa's `WebExtensionCocoa.mm` additionally uses `NSBundle`,
Security.framework resource validation and macOS-specific bundle metadata;
these need native import/verification equivalents where relevant. Native
macOS Safari App Extensions contain native platform code and use Safari/macOS
APIs. Running their WebExtension resources is not evidence that Summit runs
those native components. That part of the full Safari support objective still
requires a separate platform compatibility implementation or demonstrated
equivalent behavior; it cannot be silently marked solved by this route.

The first runtime acceptance fixture should load an MV2 extension and an MV3
extension through the real controller, load an ordinary HTTP fixture page,
observe an isolated content script, exchange a message with a background
document/worker, round-trip extension storage, revoke a host grant, unload and
restart. It should prove that a page cannot call privileged APIs and that two
extensions cannot share data or privileged identifiers. Subsequent fixtures
cover popups/tabs/windows, navigation, subframes, workers, private windows,
network filtering, crashes and update migration. This progression is a way to
reach the full runtime, not a replacement for the real Safari/Chrome/Firefox
extension compatibility corpus required by the objective.

Existing upstream tests provide expectations to adapt rather than invent:
`Tools/TestWebKitAPI/Tests/WebKit/WKWebView/glib/TestWebKitWebExtension.cpp`,
`TestWebKitWebExtensionContext.cpp` and
`TestWebKitWebExtensionMatchPattern.cpp` cover the developing GLib facade.
The adjacent `WKWebExtension*.mm` tests cover the broader Cocoa runtime and
API families, with helpers under `Helpers/cocoa/WebExtensionUtilities.mm`.
Port their assertions and fixtures to the native embedding while retaining
API behavior; merely reproducing the small GLib test surface would leave most
of the browser's requested extension compatibility unverified.
