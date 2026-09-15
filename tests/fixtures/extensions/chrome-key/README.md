# Chrome manifest key identity fixture

`public-key.txt` contains the public (not private) RSA SubjectPublicKeyInfo
bytes from Chromium's `IDUtilTest.GenerateID`, base64 encoded for a manifest
`key`. Its expected extension ID is `melddjfinppjdikinhbgehiennejpfhp`.
The `test` and `_` byte-string vectors in the portable test use the same
upstream test's expected IDs. The test does not derive expected IDs using
Summit's implementation.

Sources inspected September 15, 2026:

- [Chromium ID test vectors](https://github.com/chromium/chromium/blob/main/components/crx_file/id_util_unittest.cc)
- [Chromium identity algorithm](https://github.com/chromium/chromium/blob/main/components/crx_file/id_util.cc)
- [Chrome manifest key documentation](https://developer.chrome.com/docs/extensions/reference/manifest/key)

Only public test data and expected values are reused; implementation and test
code are written for Summit. A manifest key identifies bytes and proves no
package signature or publisher identity.
