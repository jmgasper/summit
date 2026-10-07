# October 7 issue work

The requested scope is every open issue in `jmgasper/summit`, followed by a
verified installation on X399. The final installation is still pending.

| Issue | Current evidence / remaining work |
| --- | --- |
| #39 New icons | Supplied toolbar SVGs render at three densities; the app uses the updated mountain-and-flag HVIF. Native bundle `bundle-uofxmys5` builds. Screenshot `.vm/issue39-native.png` verifies the toolbar in the preceding icon bundle. Commit `a2a9512` is pushed. |
| #40 Extension stores | Implemented and verified against both live stores; [workflow and native evidence](extension-stores.md). |
| #41 Certificate information | Implemented: supplied lock icon and native details from the committed connection. Verified/public and exception states, tab/navigation cleanup, and resumed TLS peer certificate retention pass 72 native checks; [details](certificate-warnings.md). |
| #38 PDF viewing | Implemented with bundled PDF.js, original-response downloads, navigation/search/zoom, passwords, and error handling. 118 native checks pass; [details](pdf-viewer.md). |
| #37 README | Pending until the features and icon work finish; replace old KunanyiOS branding with air/OS, audit current capabilities, retain the CI latest-release download section. |
| #36 Date/time input types | Implemented for all five types with BCalendar and canonical editable values. 171 native checks pass, including leap dates, ISO weeks, range/step constraints, events and cleanup; [details](native-form-pickers.md). |
| #35 Color input | Implemented with BColorControl, Done/Cancel, script updates and datalist swatches. 65 native checks pass, including DOM events and navigation/tab/quit cleanup; [details](native-form-pickers.md). |
| #34 Reader mode | Implemented: address-field button, isolated Readability extraction, DOMPurify/CSP, in-memory article view, original-URL history and private browsing. 79 native checks pass; [details](reader-mode.md). |
| #33 DASH | Implemented direct H.264/AAC DASH with adaptive quality, byte ranges, seeking and live refresh. 82 native checks and portable ASan/UBSan tests pass; [scope and evidence](dash-playback.md). |
| #32 EME | Implemented temporary Clear Key CENC for H.264/AAC MSE and direct DASH, secure-context and permissions-policy checks, key lifecycle and media-origin protections. 207 native checks and portable ASan/UBSan tests pass; [scope and evidence](encrypted-media.md). |
| #31 Web Bluetooth | Implemented: 213 browser API checks, 34 full-app picker/GATT checks and 17 isolated native picker checks pass. Portable ATT/GATT, native scans and the physical busy-device test pass. Clear Key (207) and DASH (82) regressions pass on the same bundle. The user requested that the ProtoArc mouse remain connected; successful physical GATT reads are deferred. [Scope and evidence](bluetooth-transport.md). |
| #42 Latest WebKit | Added during this task. Preparing the October 7 upstream revision `fb054d09146b113aeeabee5f67f4c8b6d9379809` in an isolated checkout. Source conflict resolution, a native build and regression verification are required before changing the engine pin or installing. |

The existing installed launcher still selects `bundle-1gm22c31` with Mesa
`prefix-20261002`. Test builds use separate profiles and process groups.
Do not replace the final deployment requirement with the test bundle.

Native browser build: `SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
SUMMIT_ENGINE_BUILD_NAME=SkiaCGMiPGO bash tools/build-modern-browser-in-vm.sh
--browser --bundle --modern-extensions`. Engine build flags are recorded in
`.vm/issues-build-ninth.sh`; inspect live processes before starting another
engine build, and preserve those PGO/Skia settings for incremental work.
