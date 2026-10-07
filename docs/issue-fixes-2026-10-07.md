# October 7 issue work

The requested scope is every open issue in `jmgasper/summit`, followed by a
verified installation on X399. The final installation is still pending.

| Issue | Current evidence / remaining work |
| --- | --- |
| #39 New icons | Supplied toolbar SVGs render at three densities; the app uses the updated mountain-and-flag HVIF. Native bundle `bundle-uofxmys5` builds. Screenshot `.vm/issue39-native.png` verifies the toolbar in the preceding icon bundle. Commit `a2a9512` is pushed. |
| #40 Extension stores | Implemented and verified against both live stores; [workflow and native evidence](extension-stores.md). |
| #41 Certificate information | Implemented: supplied lock icon and native details from the committed connection. Verified/public and exception states, tab/navigation cleanup, and resumed TLS peer certificate retention pass 72 native checks; [details](certificate-warnings.md). |
| #38 PDF viewing | Pending: browser preview and optional download. |
| #37 README | Pending until the features and icon work finish; replace old KunanyiOS branding with air/OS, audit current capabilities, retain the CI latest-release download section. |
| #36 Date/time input types | Implemented for all five types with BCalendar and canonical editable values. 171 native checks pass, including leap dates, ISO weeks, range/step constraints, events and cleanup; [details](native-form-pickers.md). |
| #35 Color input | Implemented with BColorControl, Done/Cancel, script updates and datalist swatches. 65 native checks pass, including DOM events and navigation/tab/quit cleanup; [details](native-form-pickers.md). |
| #34 Reader mode | Implemented: address-field button, isolated Readability extraction, DOMPurify/CSP, in-memory article view, original-URL history and private browsing. 79 native checks pass; [details](reader-mode.md). |
| #33 DASH | Pending: adaptive MPEG-DASH playback. Audit existing MSE support and html5test detection before implementation. |
| #32 EME | Pending: Encrypted Media Extensions. |
| #31 Web Bluetooth | Pending: use air/OS Bluetooth facilities. X399 source has `headers/private/bluetooth/LEAttributeClient.h` and associated LE transport support. |

The existing installed launcher still selects `bundle-1gm22c31` with Mesa
`prefix-20261002`. Test builds use separate profiles and process groups.
Do not replace the final deployment requirement with the test bundle.

Native browser build: `SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
SUMMIT_ENGINE_BUILD_NAME=SkiaCGMiPGO bash tools/build-modern-browser-in-vm.sh
--browser --bundle --modern-extensions`. Engine build flags are recorded in
`.vm/issues-build-ninth.sh`; inspect live processes before starting another
engine build, and preserve those PGO/Skia settings for incremental work.
