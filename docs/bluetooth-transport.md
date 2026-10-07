# Web Bluetooth implementation and verification

Issue #31 is in progress. The engine patch now connects `navigator.bluetooth`,
GATT objects, origin-scoped grants, the native chooser, and the air/OS transport.
The complete native engine and Summit app build successfully. The full-app
picker/GATT suite passes 34 checks. The broader browser API suite is still in
progress; it exposed an immediate-reconnect race now fixed in the broker and
awaiting a rerun.

The engine patch contains portable Bluetooth UUID/advertisement parsing,
an ATT client, and a GATT capability boundary in
`Source/WebCore/platform/bluetooth/haiku`. The native transport lives in
`Source/WebKit/UIProcess/haiku/bluetooth/BluetoothSessionHaiku.*`.

The ATT client preserves 16-, 32-, and 128-bit UUIDs. It supports service,
included-service, characteristic and descriptor discovery; reads and reliable
long writes up to 512 bytes; write commands; and notifications/indications.
It uses the air/OS stack's 23-byte MTU. Native indications are confirmed by
the kernel. Protocol errors and transaction timeouts invalidate cached
capabilities and require the broker to close the connection.

The GATT layer exposes only granted services and checks discovered handle
ownership before every read, write or subscription. It applies the Web
Bluetooth GATT blocklist, reserves CCCD writes for notification control,
filters unsolicited notifications, and invalidates cached handles when a
Service Changed indication arrives. UUID aliases and blocklist data are
derived from the [Web Bluetooth registries](https://github.com/WebBluetoothCG/registries/tree/228b62c31c177c9b770b79896aec9ef660f62216),
with the Apache-2.0 license included beside the generated aliases.

The native layer discovers adapters through `bluetooth_server`, receives
advertisements through a private listener, and opens an L2CAP ATT socket only
after acquiring its own LE link. Scan stop, connection cancellation, and
disconnect always identify that listener. A controller already serving
another client returns busy; this code never forces an existing link closed.
It does not initiate pairing or access the OS bond-key store.

## Current evidence

`tests/EngineBluetoothTests.cpp` passes on the host under ASan/UBSan and
natively on X399. It exercises 268 assigned aliases, full UUID discovery,
512-byte reads/writes, prepare-write cancellation, notifications interleaved
with transactions, 5,000 malformed inputs, service grants, forged handles,
blocked attributes, internal CCCD access, and Service Changed invalidation.

Build it with C++20 and the `BluetoothTypesHaiku.cpp`,
`BluetoothATTClientHaiku.cpp`, and `BluetoothGATTHaiku.cpp` sources, using their
directory as an include path. No WebKit runtime or Bluetooth device is needed
for these scripted protocol checks.

`tests/NativeBluetoothTests.cpp` builds with the native session, ATT and UUID
sources plus `-lbe -lnetwork`. The X399 run found the adapter, cancelled an
active scan, and completed a fresh scan. It saw 18 advertisers in that run.
The selected ProtoArc EM11 NL is the workstation's active Bluetooth mouse.
At the user's request it remains connected to its mouse driver. The dedicated
`NativeBluetoothBusyTests.cpp --test-busy-input` check received `Busy` from the
native transport and verified that the input driver's connected status and
last-change timestamp stayed unchanged. It reads only the public live-status
archive, never stored bond keys. Successful physical GATT connection and
characteristic reads remain unverified.

The native test accepts an exact device name. It scans without connecting by
default. An additional `--connect` argument enables a connection to that
matching device and reads available battery/model/manufacturer values. It
does not read HID reports, standardized serial numbers, or unrelated devices.

## Browser boundary and test tools

The Window API includes availability, device selection, session grants,
connect/disconnect/forget, service and included-service discovery,
characteristic/descriptor reads and writes, and notification events. HTTPS
or trusted localhost, a nonopaque origin, and the Bluetooth permissions policy
are required. Device selection additionally requires user activation and a
native permission response. The broker independently validates filters, grants,
connection generations, handles, transfer sizes, and document lifetime.
Navigating or closing a view, removing a frame, or exiting its renderer cancels
pending selection and closes owned links. Cancellation identifies the exact
native prompt so a late response cannot answer its replacement.

Current scope is the top-level origin and same-origin children permitted by
policy. Cross-origin delegation, persistent grants, Web Bluetooth scanning /
advertisement observation APIs, and new pairing are not implemented. The
native stack offers one LE link per controller; another client's link remains
owned by that client. GATT services requiring a newly authenticated pairing
are not usable through this backend.

`tools/bench/bluetooth-fixture.py` serves the browser fixture on port 8775.
`tests/ModernBluetoothTests.cpp` runs it through native WebKit and Inspector,
with real pointer activation and the embedding permission messages. The
integration fixture uses an explicitly simulated ATT peripheral when both
`SUMMIT_BLUETOOTH_TEST_BACKEND=1` and `SUMMIT_ENABLE_INPUT_SYNTHESIS=1` are set
in the host process. This mode replaces native scan results, uses reserved
negative adapter identifiers, and never opens a hardware link. It retains the
ordinary chooser, origin checks, IPC, GATT capability checks and ATT parser.
Normal browser launches have neither test setting. Simulated-device results
must not be reported as successful physical-device reads.

`tests/NativeBluetoothPickerTests.cpp` exercises the actual Summit permission
service and native window without opening a hardware device. All 17 checks
pass on X399: visible device rows, Connect, cancellation by exact request ID,
duplicate-request isolation, stale-answer rejection, no generic saved decision,
and close/reopen. Visual inspection caught an initially collapsed list; the
picker now measures its minimum row height from the UI font before attachment.
The final screenshot (`.vm/issue31/picker.png`) shows both selectable devices
and the session-lifetime notice. No new crash reports or debugger events were
recorded. This native UI check uses an isolated embedding context and does not
substitute for the still-pending complete browser GATT suite.

`tests/ModernBrowserBluetoothTests.cpp` adds coverage through the complete
Summit app: a page click opens the native picker, Cancel rejects the JavaScript
request, Connect allows a simulated battery read, and navigation/quit dismiss
pending selection. All 34 checks pass on X399 with `bundle-9wi5o0ag`; no new
crash reports, debugger events, or surviving owned processes were recorded.
The fixture also checks that UUID
names and device filters retain embedded NUL bytes rather than silently matching
a truncated value.

The API run passes exposure, filtering, grants, frame policy, GATT discovery,
512-byte reads/writes, descriptors and notifications, but initially failed when
reconnecting immediately after disconnect. The old native session was still
closing and appeared to own the peer. The broker now reserves the peer for the
replacement connection and reuses its serial work queue, so cleanup runs before
the replacement opens a link. Active connections still reject competing pages.
This change requires the complete API rerun before issue #31 can be closed.
