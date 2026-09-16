# Extension identity

For [verified CRX3 packages](extension-crx.md), the signer determines identity
before consent and activation, overriding manifest declarations. For other
packages, the installer resolves identity from the prepared manifest. An existing `browser_specific_settings.gecko.id` takes precedence
over `applications.gecko.id`. A declared Gecko ID takes precedence over a Chrome
key, preserving Summit's existing behavior for mixed manifests. Otherwise the
manifest `key` determines the Chrome ID. Packages with neither declaration
retain their generated local installation identity.

For the Chrome path, Summit decodes the base64 key, hashes the bytes with
SHA-256, and encodes the first 16 bytes as 32 letters in `a` through `p`. This
matches [Chromium's identity algorithm](https://github.com/chromium/chromium/blob/main/components/crx_file/id_util.cc).
[Chrome's manifest documentation](https://developer.chrome.com/docs/extensions/reference/manifest/key)
describes how this key preserves an ID for origins and extension communication.
Summit accepts the documented single-line base64 form and conventional public
PEM wrappers; malformed declarations fail instead of silently becoming local
IDs. Inputs are bounded by 1 MiB, matching the installer's manifest limit.
OpenSSL's crypto library supplies decoding and SHA-256, and is linked explicitly
by the portable, Makefile and frozen native browser builds.

An ID derived from a public key does not authenticate a package or publisher.
CRX3 signature verification is now implemented and tested separately; authenticated
store updates remain unfinished. Mixed unsigned manifests with both Gecko and
Chrome identities still need an explicit format choice or import provenance to
select Chrome semantics. Verified CRX identity does not yet select all Chrome API
result conventions. Existing
installations keep their recorded IDs; this change does not migrate previously
installed unkeyed/local identities. Extension resource URLs still use WebKit's
separate base URL, so this does not add `chrome-extension://` URL aliases.

## Verification

The original `ExtensionIdentityTests.cpp` suite passes **29 checks** on Linux and
native Haiku. The CRX follow-up extends the host suite to **34 checks**; its
separate native SDK/browser run verifies signer identity through installation
and execution.
The known public-key ID and two byte-string IDs come from
[Chromium's independent test vectors](https://github.com/chromium/chromium/blob/main/components/crx_file/id_util_unittest.cc).
Tests also cover PEM wrapping, local/Gecko fallback and precedence, malformed
base64 and key types, unsafe identities and oversized input. All three portable
CTest suites pass.

The native browser run
`.vm/modern-extension-manager-dc50e6ea56b192358f44ed46/result.json` passes
the 29 identity checks and **123 native manager/picker checks** on
`bundle-mo8zo6d4`. A manifest containing Chromium's public test key and no Gecko
ID is installed through the actual file picker. Its real background script
reports `chrome.runtime.id` as `melddjfinppjdikinhbgehiennejpfhp`, with saved
storage, restored required grants and no optional `tabs` grant. Disable/enable,
duplicate identity rejection, another browser process, removal, and a final
restart all pass. Teams 33192, 33308 and 33369 exit with status 0; their process
groups drain without forced cleanup, the crash-log interval is clean and all
input hashes remain unchanged. The engine patch remains `2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`.

The existing Gecko-ID package also passes all 123 native picker/lifecycle
checks on that same bundle in
`.vm/modern-extension-manager-6e025759766b1b92a3c86046/result.json`, with clean
process exits, drained groups, a clean crash-log interval and unchanged inputs.

The native system-WebKit Makefile target also compiles and links with the
explicit crypto dependency, and its 38 core checks pass. Evidence is in
`.vm/legacy-chrome-identity-build-result.json`; this verifies that build path,
not extension execution in the legacy backend.

The verified modern bundle and matching engine sources are available at
`artifacts/modern-browser/bundle-mo8zo6d4`. Its copy provenance digest is
`f0fc447700311f16763ae5f28f85b3f4fc8175b0a7b715c516da16dbe2cd6fbd`.
See [copy and rebuild details](modern-bundle-copy.md).

```sh
ctest --test-dir build-host --output-on-failure
python3 tools/test-modern-extension-startup.py --manager --chrome-key --bundle /absolute/guest/bundle
```

These checks establish keyed identity and the tested lifecycle, not complete
Chrome API, CRX, URL-scheme or extension-store compatibility.
