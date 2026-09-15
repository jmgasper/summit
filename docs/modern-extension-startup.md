# Installed extension startup

The modern extension-enabled Summit browser now loads enabled installations
from its profile's `Extensions/catalog.json`. `ExtensionController` prepares
each owned package asynchronously, compares the returned fingerprint with the
approved catalog record, and loads it with the public SDK's `BrowserStartup`
purpose. WebKit restores grants only when its saved resource identity matches.
The controller retains the actual load receipt, base URL and per-entry error.

Disabled packages are skipped. A changed package requires new approval; its
failure does not prevent later valid entries from loading. Startup does not
rewrite the catalog. The catalog accepts both uppercase and lowercase hex and
preserves the SDK fingerprint exactly, since approval compares it verbatim.

Application shutdown cancels pending preparation, handles a successful load
receipt that arrives during closing, unloads loaded extensions, and waits for
preparation workers before releasing the controller and browser context. After
the native application has been destroyed, the modern entry point flushes
stdio and calls `std::_Exit`. This matches the Haiku WebKit helper-process exit
behavior: remaining worker TLS destructors must not race destruction of libbe's
global handler-token table. Normal window, profile and context cleanup still
runs before that final exit.

The installer/consent UI, extension manager, update and removal flows are not
implemented yet. The controller does not provide an operation timeout if an
SDK reply is lost. Its normal startup/shutdown path is tested; cancellation
while loading and prolonged use need further integration coverage. This work
does not establish general Safari, Chrome or Firefox extension compatibility.

## Native verification

`tools/test-modern-extension-startup.py` operates on a frozen full-browser
bundle. A separate setup application uses the actual public preparation and
approval APIs plus Summit's catalog. The extension's background script writes
an initial boot counter and nonce, checks granted permissions, and sends an
HTTP report to an isolated local fixture server. A dedicated extension view
confirms the first report before setup unloads the extension and exits.

The runner then launches two distinct Summit processes with the saved profile.
Each must send a fresh background report with restored permissions, the same
nonce, and boot counters 2 and 3. Disabled and deliberately changed records
precede the valid installation. Both browser runs must skip/reject those
records, load the valid package, preserve the catalog, and close normally.
The runner checks actual kernel team membership for each owned process group,
monitors new native debugger events, and rechecks bundle and test input hashes.

The run in `.vm/modern-extension-startup-f03901c8f1ddafb59a72746f/result.json`
passes on `/boot/home/summit/build-modern-browser/bundle-w5i95j6u`, using engine
patch `2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`.
Setup team 31377 and browser teams 31419 and 31474 exit with status 0; all
groups drain without forced cleanup and the crash-log interval is clean.
This is process-restart evidence, beyond the earlier same-context SDK test.

The preceding exit-fixed bundle `bundle-d9i3_xxt` passed five consecutive
90-check navigation runs and all 143 native close checks with clean monitored
exits. The only subsequent application change in `bundle-w5i95j6u` is allowing
uppercase catalog fingerprints. That catalog change passes 57 host and 57
native checks, including rejection of nonhex digits and exact round-trip
preservation. Evidence is in `.vm/browser-process-exit-regressions-result.json`
and `.vm/extension-catalog.UuD5DdlP/result.json`.

Preserved failures:

- `bundle-n9f4t1op` failed shutdown in `BTokenSpace::RemoveToken` while the
  WebsiteDataStore worker's RunLoop TLS destructor was finishing. The failing
  navigation run and stack are in
  `.vm/modern-load-errors-456508daf8e2c941ad9d9fed/`.
- The initial startup setup failed because the catalog rejected the SDK's
  uppercase digest. Evidence:
  `.vm/modern-extension-startup-d588f0b5379524431c50a298/result.json`.
- A subsequent harness version incorrectly used `killpg(group, 0)` to detect
  live helpers. On Haiku it can succeed with no live teams. That run remains
  recorded as failed in
  `.vm/modern-extension-startup-4d12016532e05adfd8fa68e2/result.json`; the final
  runner uses the kernel's live team list and a fresh profile.

To repeat against an existing frozen guest bundle:

```sh
python3 tools/test-modern-extension-startup.py --bundle /absolute/guest/bundle
```

Run it serially with other native browser tests because debugger-event
monitoring covers the whole Summit VM. The user-facing preview can be followed
through VNC at `127.0.0.1:5905` on the host.
