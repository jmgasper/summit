# Reader mode

Issue [#34](https://github.com/jmgasper/summit/issues/34) adds a book button
inside the right edge of the address field. After an HTTP(S) HTML article
loads, a lightweight Readability probe determines whether to show the button.
Clicking it extracts the article into a narrow, responsive reading view with
headings, paragraphs, lists, tables, images, source link and author metadata.
The view follows the system's light/dark preference and supports normal browser
zoom. Clicking the book again returns to the original page.

The address field, bookmarks, saved sessions and history retain the original
web URL. The generated document has its own unguessable internal origin, and
the connection-certificate button is absent for that generated page. One
extracted article is kept in memory per tab; reloading or returning with Back
can display it again. If another article has replaced that cache, older Reader
history entries reopen their original web page. Closing the tab releases its
article. Private windows create no reader files or saved history entries.

## Extraction and content boundaries

[Mozilla Readability](https://github.com/mozilla/readability) extracts the
article; [DOMPurify](https://github.com/cure53/DOMPurify) sanitizes it. Their
pinned source URLs and SHA-256 hashes are in
[`resources/reader/sources.json`](../resources/reader/sources.json). Both are
embedded as application resources and distributed with their Apache 2.0
license notices. Reader mode does not download executable code.

The new `BWebKitView::EvaluateJavaScriptIsolated` embedding method creates a
fresh content world for each inspection. Page globals, modified prototypes
and named-property overrides cannot replace the extractor or sanitizer.
Inspection neither grants nor consumes user activation. Both the engine and
browser reject results from a superseded navigation.

Extraction uses a cloned document, a 50,000-element limit and a bounded result.
The sanitizer allows only ordinary article elements and a small attribute
list. Links and images resolve against the original page and must use HTTP(S).
Scripts, handlers, forms, embedded documents, SVG/MathML markup, styles and
unsafe URLs are removed. The reading document also enforces a restrictive
Content Security Policy and suppresses referrers. Only the owning tab may
retrieve its exact internal URL as a top-level document; it is unavailable to
subframes or cross-origin fetches.

Readability is heuristic: short pages, web applications and some unusual
article layouts do not show the button. Extraction uses the currently loaded
article, including content available through the user's existing session.

## Native validation

`tests/ModernReaderTests.cpp` uses the real browser, native address button and
native inspector against `tools/bench/pages/reader-article.html`. The fixture
deliberately overrides page-world extraction functions and includes unsafe
markup. Checks cover actual rendered content, CSP enforcement, relative links
and images, reload, Back, tab switching, exit, interrupted extraction and
private browsing. Compile with `-std=c++23 -Isrc -Ivendor -lbe`, launch an owned
browser with `SUMMIT_ENABLE_INPUT_SYNTHESIS=1`, and run:

```sh
test-reader TEAM EXECUTABLE FIXTURE_URL SCREENSHOT_PPM PROFILE_DIRECTORY
```

X399 validation on 7 October 2026: bundle `bundle-45kot2e6`, engine patch
`d692fc25f7a1261867753d8909f41014dfd478f7b2d292b5690dc86cc8d123f8`.
All 79 native checks pass. Owned team 57985 and its helpers exited normally,
with no new crash reports or debugger events. The rendered screenshot was
inspected. Local evidence: `.vm/issue34-native-results.json` and
`.vm/issue34-reader.png`. All five portable CTests also pass.
