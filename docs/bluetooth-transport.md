# Bluetooth transport work

Issue #31 is in progress. These transport components do not yet expose
`navigator.bluetooth`; the WebIDL, browser broker, chooser, and browser-level
verification remain to be connected.

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
The selected ProtoArc EM11 NL was not advertising under its supplied name;
physical connection and characteristic reads remain unverified.

The native test accepts an exact device name. It scans without connecting by
default. An additional `--connect` argument enables a connection to that
matching device and reads available battery/model/manufacturer values. It
does not read HID reports, standardized serial numbers, or unrelated devices.
