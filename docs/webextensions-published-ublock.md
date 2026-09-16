# Published uBlock Origin runtime baseline

The unmodified uBlock Origin 1.74.0 Chromium CRX installs through Summit's
native package picker and signed-package consent. Its first runtime test fails:
the toolbar title remains the static manifest title instead of receiving the
background's request count. Filtering, popup interaction and restart behavior
have not passed. A subsequent console trace identifies the first startup error:
the published wrapper accesses the missing `chrome.privacy` namespace.

The package is pinned in `compatibility/extensions.lock.json` with SHA-256
`b6be71ed3e3e85eaad8f02710b9071d06428e141d942c43d5f65d4526e82dc3e`.
Its verified signer identifier is `fkgkibajhfbepljeaefdnfnegdcjomkh`. The test
installs the original CRX bytes without modifying or instrumenting the extension.

Run against an idle native bundle in the Summit QEMU instance:

```sh
python3 tools/test-modern-ublock.py \
  --bundle /SummitExtensions/summit/build-modern-browser/bundle-d0h_6xzg \
  --watch
```

`--watch` pauses at visible UI steps for VNC viewing. `--compile-only` checks
native harness compilation without claiming runtime compatibility. Close other
instances of this bundle before the runtime test; the runner verifies that the
browser and its helper executables have no existing teams.

The controlled page loads a normal script and `ads/custom_ads.js`. The latter
matches the exact `/ads/custom_ads.js$script` rule in the package's bundled
EasyList. Before installation, both scripts must reach the server once and
execute their unique response bytes. For filtering to pass, the normal script
must still execute while the matched script produces a load error and never
reaches the server. The server records requests and serves the same resource
behavior in every phase. No server-side filtering substitutes for the extension.

The remaining planned runtime phases exercise the published popup's filtering
switch, native disable/reenable and process restart. They are already encoded
in the harness but were not reached by this baseline. In particular, the popup
power-switch click position still needs visual validation once the popup loads.

The baseline report is
`.vm/modern-ublock-c65e01151549142212b13422/result.json`, with native stage
`/SummitExtensions/summit/extension-ublock-browser.G5V0do4d`. It records:

- Successful native compilation, baseline resource loads, signature consent,
  installation, version and signer checks.
- Failure after 45 seconds waiting for a title beginning `uBlock Origin (`.
  The observed title was `uBlock Origin`; the diagnostic popup screenshot was
  blank. This does not identify the specific JavaScript exception.
- Normal browser exit, no remaining owned processes, no forced cleanup and a
  clean native crash-log interval (`501698` through `502544`).
- Unchanged frozen bundle, staged inputs, host sources and retained CRX bytes.

This used bundle `bundle-d0h_6xzg`, whose engine patch is
`d5887e4f3e31b5c580a90cd501a9917853bfffccd32fe5a239fe6a8da6bd809c`.
The earlier compile-only report is
`.vm/modern-ublock-5283e496bf4c0c888c5b0882/result.json`.

Source inspection confirms that the current webRequest implementation rejects
blocking listeners. Its request notification does not wait for extension replies
before continuing to the network. uBlock registers blocking listeners during
background module initialization. This remains a compatibility gap after the
earlier privacy error. Supporting filtering requires request interception before
network dispatch, including permission and lifecycle handling. Accepting the
listener option while ignoring its return value would not satisfy the test.

## Console diagnosis

The Haiku extension page configuration now enables WebKit's existing console
logging preference when `SUMMIT_TRACE_EXTENSION_CONSOLE` is exactly `1`. It is
off by default and changes no extension package bytes. The uBlock runner exposes
this as `--trace-console`, records the diagnostic mode and keeps
`runtime_verified` false in that mode.

```sh
python3 tools/test-modern-ublock.py \
  --bundle /SummitExtensions/summit/build-modern-browser/bundle-z9d_5c2k \
  --watch --trace-console
```

This native diagnostic bundle uses engine patch
`a06d5e8d3d0c582fc117132d70912410adbead58e835ade904addb6778785182`.
Its engine and browser build passed, with unchanged inputs, in
`.vm/modern-browser-ublock-console-bundle-result.json`.

The diagnostic run is recorded in
`.vm/modern-ublock-b795dc8a66cd99f153fc94fc/result.json` (SHA-256
`172d8fa9e8b5af40d164804e98537720900a9a9123f4f6058d1e2175d41ec566`).
The first error is at `js/webext.js:144:39`: a `TypeError` evaluating
`chrome.privacy[category]`. The exact published `js/webext.js` has SHA-256
`173d7475ba50943ca3a12cbc98410525a0423b278e70f67877d56753366b2d30`.
Its wrapper reads three privacy settings before background module initialization
can finish. The later popup errors include missing runtime connection listeners,
consistent with the background failure. Browser shutdown and the native crash-log
interval (`509584` through `510249`) were clean; bundle, inputs and retained CRX
bytes were unchanged.

With diagnostics disabled, the same bundle passes all **78 published Dark Reader
checks**, including real popup On/Off input and native disable/reenable. The
report is `.vm/modern-darkreader-39c1f5899395d0975d3752e6/result.json` (SHA-256
`2ffd782c50934ef883b23d61d35e63a7a3b182978beb3cbfbf8709b5d7901182`).
It records normal exits, drained processes, no forced cleanup, a clean crash log
and unchanged sources, bundle and package bytes.

The next implementation needs real privacy controls. The published wrapper
expects `network.networkPredictionEnabled`, `network.webRTCIPHandlingPolicy`
and `websites.hyperlinkAuditingEnabled`. The API requires the `privacy` permission
and setting operations with appropriate control levels; see the
[privacy API](https://developer.chrome.com/docs/extensions/reference/api/privacy)
and [setting lifecycle](https://developer.chrome.com/docs/extensions/reference/api/types).
The current upstream C setters for hyperlink auditing and DNS prefetching are
deprecated no-ops. Wiring the API to those setters would not implement the
requested behavior. Privacy support and uBlock filtering remain unfinished.
