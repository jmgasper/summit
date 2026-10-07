# Encrypted Media Extensions

Summit implements `navigator.requestMediaKeySystemAccess()`, `MediaKeys`,
`MediaKeySession`, encrypted initialization-data events, key status changes,
and `waitingforkey` through WebKit's Encrypted Media Extensions API.
The native CDM supports **`org.w3.clearkey`**, temporary sessions, H.264 and
AAC-LC in CENC-encrypted fragmented MP4. Applications can use MediaSource or
a direct MPEG-DASH media URL. EME is available in secure contexts, including
HTTPS and trusted localhost. The `encrypted-media` permissions policy defaults
to the same origin. Iframe `allow` attributes and `Permissions-Policy` response
headers can further restrict access; cross-origin frames need delegation.
Header allowlists support `self`, `*`, an empty list, and exact quoted origins.
Wildcard host/port expressions and policy-reporting endpoints are not implemented.

Direct DASH tracks every media response's origin and CORS result. Cross-origin
media without CORS permission exposes an empty encrypted event and taints
canvas, even when the manifest itself is same-origin. Set the media element's
`crossOrigin` attribute and serve appropriate CORS headers to permit access.

Clear Key is the interoperable key system defined by the
[EME specification](https://www.w3.org/TR/encrypted-media/#clear-key).
It does not provide Widevine, PlayReady, FairPlay, hardware robustness,
persistent licenses, or commercial streaming-service certification. Those
key systems and unsupported configurations are rejected. Keys remain in
memory and are removed when their temporary session closes or is removed.

## Supported encryption

The fragmented MP4 reader accepts `encv`/`enca` entries with an H.264/AAC
original format, `cenc` scheme and `tenc` defaults. It reads inline `senc`
metadata and `saiz`/`saio` auxiliary data, with 8- or 16-byte IVs and clear /
encrypted subsample ranges. AES-128 CTR uses OpenSSL's EVP implementation.
Malformed metadata fails before ciphertext reaches a decoder.

Common-SystemID version-1 PSSH data in movie and fragment boxes becomes a
`cenc` encrypted event. `keyids` initialization data is also supported.
License requests include the requested IDs and the `temporary` session
type, and license updates accept a JSON Web Key Set. The
[W3C CENC initialization-data format](https://www.w3.org/TR/eme-initdata-cenc/)
defines the Common SystemID and PSSH representation.

Key lookup and decryption run on the existing decoder threads. Missing keys
leave encrypted samples in bounded queues, trigger `waitingforkey`, and
stop the playback clock. Delivering keys or attaching the correct MediaKeys
object resumes decoding. Seeking, unloading and shutdown wake those queues.
Samples retained by MediaSource remain encrypted; a decoder receives a
separate plaintext copy only when a usable key is available.

Limitations: progressive encrypted MP4 outside DASH/MSE, WebM encryption,
CBCS/CBC1/CENS, pattern encryption, per-sample `seig` key overrides, legacy
`senc` overrides, multiple sample descriptions / clear-lead configurations,
and noncontiguous auxiliary-data offset tables are not supported. CENC
support does not imply support for every feature in the ISO BMFF container.

## Verification

Verified on X399 with frozen bundle `bundle-5a2zwdja`: all **207 native
checks** passed, with no remaining helpers or new crash reports. The engine
patch SHA-256 was
`b1d1c7dbc87106f8eddd67cdad557aa1bfe45e18e0117ae3437bbe66439ae401`.
The captured screen shows decrypted video; native traces confirm NVDEC video
decoding and 48 kHz AAC audio output. Tests cover secure-context exposure,
iframe and response-header policies, negotiation, malformed licenses, key
sharing and revocation, delayed key delivery, media-element attachment,
pause/seek/end, direct encrypted DASH, cross-origin metadata and canvas
protection, clear MSE, and shutdown while waiting for a key. All 82 native
clear-DASH regression checks also pass in this bundle.

`tests/EngineCENCTests.cpp` tests the [NIST SP 800-38A F.5.1 AES-128 CTR vector](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf), subsample
keystream continuity, malformed IVs and ranges, and byte-for-byte recovery
of Shaka Packager's encrypted H.264/AAC samples. It exercises fragmented
input, inline and auxiliary-only metadata, 8/16-byte IVs, and 4,000 mutated
metadata inputs under ASan/UBSan. The test recovers 2,375 samples. The clear DASH playback regression also
passes after the shared parser changes, delivering 2,280 video and 3,444 audio
samples across seek, live-window and multi-period scenarios.

Generate the interoperability fixtures with FFmpeg and Shaka Packager:

```sh
python3 tools/bench/eme-fixture.py --directory .vm/eme-fixture \
  --packager /path/to/packager
```

Run the fixture server on the native test machine:

```sh
python3 tools/bench/eme-fixture.py --directory /path/to/eme-fixture --serve
```

Open `http://127.0.0.1:8773/test.html`. The automated native harness is
`tests/ModernEMETests.cpp`; it uses a private context, real Media Kit decoders,
native input, and the Web Inspector protocol to verify page-visible results.

For the ordinary-HTTP negative test, bind the server with `--host 0.0.0.0` and
set `SUMMIT_EME_INSECURE_ORIGIN` to its LAN HTTP origin. The harness also uses
`localhost` as a second origin for iframe and media CORS tests.
