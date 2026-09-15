# Native dynamic action icons

The native icon suite passes against `bundle-_inqiw0r`: **47 API cases, 53 SDK
and desktop pixel observations, and 439 native checks**. The owned browser and
helpers exit cleanly, the crash monitor passes, and the frozen bundle and staged
inputs remain unchanged. The same bundle also passes 238 action/popup checks
and 169 overflow checks with clean native exits and no crash events.
The full browser and arbitrary extension compatibility remain unfinished.

The verified bundle is preserved at
`artifacts/modern-browser/bundle-_inqiw0r`. Copy provenance is
`1474b16a3a8c78464c5067e44d7c4769d1fed85af7959c5566b0cdf5b19a4dbb`;
the matching WebKit source archive digest is
`55292ce7df17670c59740adb716631477e3975e3ce0f8be52678bf2f50f6a49f`.
The copy records host revision `6cc95e9`, after native implementation commit
`e94637d`, and verifies 399 artifact entries. Source and support fingerprints
match before and after the copy.

QEMU's VNC display `127.0.0.1:5905` runs the same native bundle. Its owned preview
uses a fresh copy of the passing icon-test profile; the 22 source-profile files
remain unchanged. `.vm/vnc-follow-along-browser.json` records its live ownership
and profile. Close that owned preview before another exclusive native suite.

## Behavior

`setIcon` accepts paths, size-to-path dictionaries, genuine `ImageData`, and
size-to-`ImageData` dictionaries. The binding retains raw JavaScript values and
uses native wrapper checks, including when constructors, dimensions, or typed
array properties are shadowed. It copies image buffers before encoding PNG for
IPC, so subsequent JavaScript mutations cannot change the installed icon.

Relative paths resolve against the calling extension frame. Rooted paths and
same-extension absolute URLs use the package resource loader. External URLs are
rejected. Native decoders handle bitmap and SVG data URLs, including SVG media
types without a semicolon.

SVG rendering creates WebCore's isolated image document. The UI process now
provides the required platform loader, which accepts only inline data resources
in SVG image frames and completes their decoding before rasterization. Scripting
and media stay disabled. SVG files, scripted SVG isolation, embedded PNGs, and
malformed embedded images are covered by the native pixel suite.

The tests also exposed a Haiku image-buffer mismatch: B_RGBA32 stores straight
alpha, while the common pixel helpers assumed premultiplied storage. The Haiku
backend now uses the native storage format for reads and writes and synchronizes
its BView before accessing bitmap memory. Coverage includes translucent icons,
canvas painting, exact native canvas readback with clipping, SVG alpha, and PNG
alpha inside SVG. The readback fixture exceeds WebKit's 60x60 `putImageData`
cache so it exercises the native backend directly.

The UI receiver independently validates the serialized dictionary and target
access, decodes every representation before mutation, and preserves the old
icon on failure. Successful changes use the existing inheritance, invalidation,
observer, SDK export and toolbar drawing paths.

Null, undefined, omitted and empty dictionaries reset the selected scope. Tab
and window targets cannot be combined. Only known detail properties are read;
inherited properties are accepted and throwing getters/proxies are rejected.
These inputs and resets follow the [Firefox API](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/browserAction/setIcon)
and the [Chrome API/callback contract](https://developer.chrome.com/docs/extensions/reference/api/action#method-setIcon).

Limits are 16 Mi characters of serialized icon JSON, 256 representations and
64 MiB per ImageData buffer. Size keys are canonical positive 32-bit integers.
The upstream icon-variants feature remains disabled on Haiku. Cocoa compilation
and runtime have not been tested.

## Evidence

Engine patch:
`e71fe35768f9d28d5475179f0d782b7820e3027bdfb48f2bea1036ff583b65ca`.

- `.vm/modern-browser-action-icons-alpha-bundle-result.json`: immutable native
  browser bundle built from the completed engine.
- `.vm/modern-extension-icons-84b00bb1c07e04c0733b6753/result.json`: passing native
  icon suite, clean process exits and crash monitor, unchanged source inputs and
  bundle. Native stage: `/boot/home/summit/extension-icon-inputs.gjmB8MWG`.
- `.vm/modern-extension-actions-794d107692da6f7924ff2192/result.json`: 238
  action/popup checks over installation, popup quit, removal and restart phases.
- `.vm/modern-extension-overflow-e5498aeb8666afc19bb4f864/result.json`: 169 native
  overflow checks. Both regression reports preserve bundle/staged input hashes,
  record clean browser/helper exits and have passing crash monitors.
- `.vm/action-icon-alpha-engine-build.log`: successful WebCore, WebKit,
  NetworkProcess and WebProcess build/link after the alpha correction.
- `.vm/extension-lifecycle-inputs.YNxUk7WV/result.json`: isolated native compile
  of the SVG loader; no warnings.
- `.vm/extension-lifecycle-inputs.SRwK7L7Z/result.json`: six action API, parser,
  receiver and generated-binding integration units compile.
- `.vm/action-icon-promoted-binding-validation.json`: 510 generation/preprocessor
  checks against the earlier `84d3e1b1...` patch. Later changes affect only native
  SVG loading and image-buffer storage, not the bindings.

Tests drive real installation controls and toolbar clicks. Each observation
checks decoded SDK pixels and alpha-composited desktop pixels before advancing.
Coverage includes paths, representation selection, image snapshots, callbacks,
invalid-input atomicity, inherited state, tab switching, navigation resets and
closed-tab rejection, alongside the rendering cases above.

```sh
python3 tools/test-modern-extension-icons.py --watch --bundle /path/to/native/bundle
python3 tools/test-modern-extension-startup.py --manager --actions --chrome-key --watch \
  --bundle /path/to/native/bundle
python3 tools/test-modern-extension-overflow.py --bundle /path/to/native/bundle
```

Run these suites serially after closing the owned VNC preview and draining its
helpers: crash-log observation is global to the VM. Failed bundles remain
preserved. The first run's manifest SVG crash is recorded in
`.vm/action-icon-svg-crash.report`; subsequent SVG/alpha diagnostic runs and the
passing icon run have clean native exits and crash logs.

Further work includes malformed SVG document rejection, external SVG resource
coverage, private/multiwindow scenarios, programmatic `openPopup`, MV3 service
workers, and representative Safari/Chrome/Firefox extensions. Passing this
fixture does not establish arbitrary third-party extension compatibility.
