# Published extension compatibility corpus

These are unmodified official release packages selected for native Summit
compatibility testing. Download and archive inspection are complete. **None has
passed a native Summit runtime test.** They complement the focused API fixtures;
successful fixtures do not establish compatibility with these extensions.

| Project | Pinned release | Package | Manifest |
| --- | --- | --- | --- |
| uBlock Origin | [1.74.0](https://github.com/gorhill/uBlock/releases/tag/1.74.0) | Chromium CRX | V2, background page |
| uBlock Origin | 1.74.0 | Chromium ZIP | V2, background page; manifest is inside `uBlock0.chromium/` |
| uBlock Origin | 1.74.0 | Signed Firefox XPI | V2, background page, Gecko ID `uBlock0@raymondhill.net` |
| Dark Reader | [4.9.131](https://github.com/darkreader/darkreader/releases/tag/v4.9.131) | Chromium MV3 ZIP | V3, `background/index.js` service worker |
| Dark Reader | 4.9.131 | Chromium MV2 ZIP | V2, persistent background page |
| Dark Reader | 4.9.131 | Firefox XPI | V2, background page, Gecko ID `addon@darkreader.org` |

Exact release URLs, byte lengths and SHA-256 digests are in
[`extensions.lock.json`](extensions.lock.json). Each digest matches the publisher's
GitHub release-asset metadata. This checks downloaded bytes; it does not verify
an embedded CRX or Mozilla signature.

```sh
python3 tools/fetch-extension-corpus.py
```

The tool retains packages in `.cache/extension-corpus/packages/<sha256>/` and
writes `.cache/extension-corpus/inspection.json`. It verifies the pins, reads
ZIP members without extracting or executing extension code, identifies the
package manifest, and records license-file hashes and textual API references.
Existing artifacts are checked before reuse. The inspection completed for all
six pins; `.vm/extension-corpus-fetch.log` records the download result.

Textual references include comments, feature checks and optional branches, and
can miss aliases or computed properties. They are evidence for choosing runtime
tests, not a dependency graph or a compatibility score. In particular, Dark
Reader's background and UI scripts mention `i18n.getMessage` and
`i18n.getUILanguage`; the current Haiku namespace IDL still restricts `i18n` to
Cocoa. Its MV3 package also requires the unfinished service-worker path.

Native testing must establish installation and identity, background startup,
popup UI, the extension's actual page behavior, persistence, permissions and
unload/restart behavior. uBlock should be exercised against controlled allowed
and blocked requests; Dark Reader should be checked for actual document style
changes and restoration when disabled. Failed or unsupported cases must remain
visible in the result matrix.

Safari WebExtension packages and broader extension coverage are still missing.
This initial corpus does not reduce the original Safari/Chrome/Firefox scope.
