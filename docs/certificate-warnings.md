# Sites with certificates Summit cannot verify

Added 30 September 2026 for [issue #1](https://github.com/jmgasper/summit/issues/1):
a Ubiquiti router on the local network redirects to `https://192.168.1.1` with a
self-signed certificate, and Summit only said "Could not load
https://192.168.1.1: Peer certificate or SSH remote key was not OK".

## What the user sees

- A page whose server certificate fails verification (self-signed, expired,
  issued for another name or by an unknown authority) no longer just fails in
  the status line. When the page was the one asked for (the main frame, before
  it replaced the old page), a warning opens over the window: **This
  connection to 192.168.1.1 is not private**, why the certificate was refused
  (OpenSSL's reason, for example "self-signed certificate"), who it is issued
  to and by, what names it is valid for, its validity dates and its SHA-256
  fingerprint.
- **Go Back** (the default, and Escape) leaves the tab on the page it showed
  before. **Continue to 192.168.1.1** trusts that certificate for that host
  and loads the page.
- The choice is remembered. Summit keeps the host and the certificate's
  SHA-256 in the profile (`trustedCertificates` in `profile.json`), so the
  site opens directly from then on, also after a restart and in private
  windows. Only that certificate is trusted: if the site's certificate
  changes, the warning comes back. A choice made in a private window lasts
  only for the private session.
- A background tab that fails this way asks when it is selected. A request of
  a page that has already loaded (an image, a script, a WebSocket to another
  host) is refused without a question; the status line names the reason.
- **Preferences › History and Data** shows how many such certificates are
  trusted and has **Forget Trusted Certificates…**, after which the warning
  shows again.

## How it works

The Haiku network process uses libcurl. When a TLS handshake fails
verification, `NetworkDataTaskCurl` (and `WebSocketTask`) asks the UI process
through a server-trust authentication challenge before giving up; the Haiku
port answered none of them, so every such load failed.

- `WebView.cpp`'s navigation client now answers server-trust challenges. It
  computes the leaf certificate's SHA-256 from the challenge's certificate
  chain; if the page's `BWebKitContext` trusts that certificate for the host,
  it answers with a credential, and the curl task repeats the request without
  verification. Otherwise it refuses, and remembers the certificate (subject,
  issuer, names, validity, OpenSSL's verification error) for the view.
- When the main frame's load then fails with curl's certificate error, the
  load error sent with `B_WEBKIT_STATE_CHANGED` carries
  `loadErrorCertificate` and `loadErrorCertificateHost`, `…SHA256`,
  `…Subject`, `…Issuer`, `…Problem`, `…Names`, `…ValidFrom` and
  `…ValidUntil` (see [modern-navigation-errors.md](modern-navigation-errors.md)).
- `BWebKitContext::AllowServerCertificate(host, sha256)` and
  `ForgetServerCertificates(host)` keep the trusted certificates of a context.
  The engine does not store them; Summit gives its saved ones to each context
  it creates.

## Not done

- There is no certificate viewer beyond the warning's summary.
- HTTP authentication (basic/digest) challenges still get the engine's
  default handling, which does not ask for a password.
