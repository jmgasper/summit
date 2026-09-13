# Original objective and acceptance evidence

Create a full web browser for KunanyiOS in this workspace, based on the latest
WebKit codebase, with a KunanyiOS/Haiku native UI resembling Safari, and full
Safari, Chrome and Firefox extension support comparable in intent to Orion.
QEMU is the initial native test environment. The preview is an intermediate
deliverable; the original objective remains active.

| Requirement | Current evidence | Still needed for acceptance |
| --- | --- | --- |
| New application in this folder | `summit/`, native executable, source and build tools | Maintain a reproducible distributable application. |
| Full web browser | Real HTTP/HTTPS pages, native tabs, navigation, search, bookmarks/history, session restore, basic downloads, find/zoom. Native fixture tests. | Full browsing feature review, multiple windows/private browsing, permissions/authentication and certificate UX, download management, settings, developer tools, storage privacy, accessibility, performance and sustained use. |
| Latest WebKit version/codebase | Upstream `00991b6cdc59937da6076c69a22ae6a9460a4445`, native port patch, successful JavaScriptCore library/shell build and runtime smoke checks including ICU and WebAssembly. WebCore compilation started. | Compile all of WebCore and embedding, run the browser against that engine, run relevant upstream conformance/regression tests, record actual engine identity and update process. The packaged 1.10.0 engine does not satisfy this requirement. |
| KunanyiOS/Haiku UI resembling Safari | Interface Kit window/toolbar, central address field, tabs, sidebar, start page; observed in QEMU. | Refine interactions and visual consistency, menus, accessibility and native integration; test KunanyiOS itself. |
| Chrome extensions | No runtime implemented. | CRX/ZIP install/update, Manifest V2/V3, isolated content scripts, background pages/service workers, `chrome.*` APIs, permissions, actions/popups/options, request filtering, storage, messaging and extension compatibility corpus. |
| Firefox extensions | No runtime implemented. | XPI install/update, Gecko manifest fields, `browser.*` Promise APIs and Firefox semantics, WebRequest and representative real add-on verification. |
| Safari extensions | No runtime implemented. | Safari WebExtension bundle/resource import and API behavior, representative Safari WebExtension verification. Native macOS Safari App Extensions are a separate binary/platform problem, not demonstrated by WebExtension support. |
| QEMU test workflow | Separate running x86_64 beta6 guest, SSH/QMP tools, tests and HTTP fixtures. | Latest-engine tests, extension isolation and compatibility tests, OS integration and packaged app regression checks. |

Extension acceptance includes adversarial checks: a page must not obtain
privileged extension APIs, one extension must not access another's data,
content scripts must respect origin grants, revoked permissions must take
effect, and archives must not escape their installation directory. Parsing a
manifest or injecting a script alone does not prove extension compatibility.

Orion's own documentation describes its WebExtensions support as incomplete
and specifically identifies Safari **web** extensions. Its architecture is
inspiration, not evidence of Summit's implementation:
https://help.kagi.com/orion/browser-extensions/macos-extensions.html
