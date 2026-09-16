# Extension script result compatibility

`tabs.executeScript` has different result contracts across browser families.
An API namespace alone does not identify which contract an extension expects.
Summit must preserve this distinction as package import and compatibility
profiles develop; its current shared Firefox-style path is not exact Chrome
Manifest V2 compatibility.

## Source findings

Firefox evaluates ordinary script and returns its completion through an async
injection method. This assimilates native promises and arbitrary thenables,
including inherited `then` methods. Results are checked with structured cloning
before transport. See Mozilla's [execution implementation](https://raw.githubusercontent.com/mozilla/gecko-dev/master/toolkit/components/extensions/ExtensionContent.sys.mjs)
and [API contract](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/tabs/executeScript).

Chromium 130's MV2 [execution request](https://raw.githubusercontent.com/chromium/chromium/130.0.6723.58/extensions/browser/api/execute_code_function.cc)
selects `kDoNotWait`. Its [script executor](https://raw.githubusercontent.com/chromium/chromium/130.0.6723.58/third_party/blink/renderer/core/frame/pausable_script_executor.cc)
passes the immediate completion to the [V8 value converter](https://raw.githubusercontent.com/chromium/chromium/130.0.6723.58/content/renderer/v8_value_converter_impl.cc).
This is a native value conversion, not `JSON.stringify`: it ignores `toJSON`,
reads own properties, converts cycles to null, and converts array buffers and
views to binary values. Unsupported object properties are omitted, unsupported
array entries become null, throwing getters become null, and DOM wrappers
become empty objects. The ordinary MV2 path does not enable special Date or
RegExp conversion. These findings define future converter tests; they do not
establish that Summit implements those Chrome behaviors.

## Routing requirements

Summit's `WebExtensionControllerProxy` currently assigns one native namespace
object to both `browser` and `chrome`. Event delivery locates that object once
per frame. Splitting the objects would also require updating event discovery,
listener identity and callback error handling.

Firefox itself exposes `chrome` as well as `browser`; its content-script
bindings use the same API object for both. A Firefox extension using `chrome`
must retain Firefox's result semantics. Likewise a Chromium extension may
use a bundled browser-namespace polyfill. Neither the spelling of the API nor
the presence of a callback reliably determines package origin.

The current catalog stores identity, fingerprint, package path and access
settings, but no complete API compatibility profile. [CRX3 import](extension-crx.md)
now preserves verified signer identity through snapshots and re-verifies the
retained CRX on startup. ZIP/XPI and directory inputs still have no authenticated
compatibility origin. A complete solution needs explicit,
persistent compatibility metadata from import through engine context creation,
with deterministic handling for generic ZIP files and unpacked extensions.
The filename extension alone is insufficient evidence for all packages.

## Thenable correction

The first native implementation only observed `JSPromise` results. The expanded
injection fixture reproduces a failure on a plain object whose `then` method
resolves to `42`: bundle `zwmpgvis` reports an uncloneable result before the
method can run. The browser exits normally and its crash interval is clean.

The correction resolves a fresh intrinsic promise with the script completion,
then clones its settled value. It does not depend on a replaceable global
`Promise` constructor or `Promise.resolve`. Existing document, context and
world checks apply while the result is pending and before returning it.

Sixteen added cases cover synchronous/delayed/nested thenables, receiver and
inherited methods, one-time getter access, first-settlement behavior, getter
and method errors, rejected/uncloneable results, noncallable `then`, an
overridden promise `then`, replacement of the global Promise constructor,
and a never-settled thenable cancelled by navigation. This extends the shared
Firefox-style contract; Chrome profile selection remains separate work.

The full native build and real browser execution are verified in
`bundle-o7hhvrm9`: 129 unique injection cases / 585 native checks, plus 78
published Dark Reader checks. See the [runtime evidence](webextensions-tab-script.md#thenable-follow-up-verification).
