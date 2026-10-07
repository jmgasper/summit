# PDF viewing

Summit displays inline `application/pdf` responses in the current tab using
WebKit's bundled PDF.js viewer. The toolbar provides page navigation, zoom,
search, outlines, and downloading the original PDF. Password-protected PDFs
prompt for their password. Damaged or empty files display an error in the viewer.

This is a reader: PDF JavaScript and annotation editing are disabled. Existing
annotations and form appearances remain visible. Printing is not offered by
this integration. A server response with `Content-Disposition: attachment`
continues to use Summit's download manager directly.

## Response and resource handling

The top-level PDF stays at its original URL, preserving navigation, history,
and the browser's certificate display. `PDFJSDocument` transfers the response
bytes already loaded by WebKit into its viewer frame. It does not fetch the
document again, so authenticated requests and private browsing use their
existing network context. The viewer's Save action passes the PDF's original
bytes to the native download manager.

PDF.js comes from the pinned WebKit source tree (`Source/ThirdParty/pdfjs`),
including its worker, fonts, character maps, localization, and WebAssembly
resources. The Haiku build compresses these resources into the engine. No CDN,
extension installation, or external PDF application is required.

The `webkit-pdfjs-viewer` resource loader accepts only compiled resource paths
for PDF viewer frames. It does not interpret URL paths as filesystem paths.
Haiku-specific configuration and styles live outside the vendored PDF.js
distribution, under `Source/WebCore/Modules/pdfjs-extras/haiku` in the engine
patch. The bundle includes PDF.js's upstream license.

The Haiku engine configuration defaults `ENABLE_PDFJS` to `ON`. When reusing
an engine build directory that previously cached it as `OFF`, reconfigure
with `-DENABLE_PDFJS=ON` before building and freezing a browser bundle.

## Native verification

`tools/bench/pdf-fixture.py` creates a three-page PDF with embedded Unicode
fonts, vector artwork, a read-only form, an internal link, and a landscape
page. It also serves encrypted, scripted, authenticated, attachment, damaged,
and empty variants. Fixture generation requires ReportLab and pypdf; Poppler
can render the generated PDFs for visual inspection.

`tests/ModernPDFTests.cpp` runs against a frozen native bundle in an isolated
private WebKit context. Its public Inspector session reads the actual PDF
iframe, and native mouse messages operate the viewer's toolbar. Downloads
are compared byte-for-byte with the original fixture, and the harness waits
for its own WebKit helpers to exit before reporting completion.

`tests/ModernBrowserPDFTests.cpp` additionally checks the full browser's
address, title, Save button, reload, Back, and persisted history. It creates a
unique download filename and removes only that verified fixture afterwards.

Build the harness with the bundle's `source/include` headers, the repository's
`vendor` include directory, and `-lWebKit -lbe`. Run it with:

```text
test-pdf BASE_URL OWNED_DOWNLOAD_DIRECTORY BUNDLE GOLDEN_PDF SCREENSHOT_PPM
```

Set `WEBKIT_EXEC_PATH` to the bundle and `LIBRARY_PATH` to its private libraries
(preceded by the workstation's Mesa prefix when applicable). Set
`SUMMIT_ENABLE_INPUT_SYNTHESIS=1` for the native pointer checks. The download
directory, fixture path, and screenshot path must be absolute paths owned by
the test run.

Verified on X399 with frozen bundle `bundle-xqmj4o2b`: 86 engine checks and
32 full-browser checks passed. The engine patch SHA-256 was
`cd91bc86ed6603dfb56b7b43a74ad2bdb5ad77a351b9920c3b838e833e5294c7`.
Both successful runs exited without remaining helpers or new crash reports.
Native screenshots were inspected for the standalone view and browser tab.
The saved fixture was 29,604 bytes, with SHA-256
`c9dc0cea9bdc6361f9d2e85b14879c907d46574c6fb234d88822c771ecf61b0f`.
The five host tests and JavaScript syntax checks also passed.

The standalone harness attaches its Inspector after the initial navigation
finishes, matching the browser's Developer Tools lifecycle. PDF.js remembers
the page for a previously viewed document; repeated fixture visits explicitly
select page one before checking its pixels and text.
