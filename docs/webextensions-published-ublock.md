# Published uBlock Origin runtime baseline

The unmodified uBlock Origin 1.74.0 Chromium CRX installs through Summit's
native package picker and signed-package consent. Its first runtime test fails:
the toolbar title remains the static manifest title instead of receiving the
background's request count. Filtering, popup interaction and restart behavior
have not passed.

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
blocking listeners and dispatches observational request events after sending
requests. uBlock registers blocking listeners during background module
initialization. This is a concrete compatibility gap; the first exception still
needs runtime diagnostics. Supporting filtering requires request interception
before network dispatch, including permission and lifecycle handling. Accepting
the listener option while ignoring its return value would not satisfy the test.
