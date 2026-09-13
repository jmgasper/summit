Native extension manifest/resource port evidence, 2026-09-14

The recorded core probe compiled **7 production extension translation
units** with `WK_WEB_EXTENSIONS=1`. Production helper execution passed **27
resource-path checks** and, in the [ZIP/XPI archive step](test-engine-extension-archives-notes.md),
**111 archive checks**. The archive helper and updated constructor have
separate compile evidence. The full-engine build keeps extensions **OFF**;
no manifest executable or browser extension runtime has been linked or run.
Complete reports:
[seven-unit native compilation](test-engine-extension-native-resources-results.json)
and [resource-path execution](test-engine-extension-resource-paths-results.json).

The [initial source extraction](test-engine-extension-manifest-core.patch)
and [five-unit report](test-engine-extension-manifest-core-results.json) are
historical evidence for the first isolated proposal. The implemented
manifest/resource adapter is represented by the seven-unit report; the
subsequent archive report records its additional helper and updated
constructor.

Production changes in `.cache/WebKit/Source/WebKit`:

| Path | Change |
| --- | --- |
| `UIProcess/Extensions/WebExtension.h` | Guard GFile with `USE(GLIB)` and add the native directory/ZIP/XPI resource-path constructor. The existing JSON/in-memory constructor remains upstream code. |
| `UIProcess/Extensions/WebExtensionMatchPattern.cpp` | Remove process-pool includes and move the complete scheme-registration method. Matching and parsing bodies are unchanged. |
| `UIProcess/Extensions/WebExtensionMatchPatternProcessPool.cpp` | Retain original scheme sets, service-worker registration and process broadcasts. This full-runtime unit is not part of the isolated compile. |
| `UIProcess/Extensions/glib/WebExtensionGLib.cpp` | Keep the GFile constructor; move three platform-independent resource/error/icon-selection bodies into the common source. |
| `UIProcess/Extensions/WebExtensionResources.cpp` | Share unchanged `resourceDataForPath`, `recordError` and `bestIcon` bodies on non-Cocoa platforms, retaining in-memory resources, generated background resources, real reads/errors and cache behavior. Cocoa retains its signed-resource validation and KVO implementation. |
| `UIProcess/Extensions/WebExtension.cpp` | Route Haiku resources through the native resolver; replace textual-prefix containment with separator-aware `FileSystem::isAncestor` on other platforms. |
| `UIProcess/Extensions/haiku/WebExtensionResourcePathsHaiku.h` | Canonicalize with explicit failure, check ancestry, return the checked canonical path and escape native UTF-8 filenames correctly. |
| `UIProcess/Extensions/haiku/WebExtensionHaiku.cpp` | Load a canonical directory or extracted ZIP/XPI resources through the upstream parser; decode bitmap/SVG icons using WebCore and create native icons. Feature/platform guarded. |
| `Sources.txt` | Include both extracted common units. Resources use `@no-unify` because preserved generated-resource constants share names with parser constants. |

The three moved GLib method bodies and scheme-registration method were
compared with pinned upstream and are byte-for-byte unchanged. No replacement
manifest parser, matcher, resource reader, error object or success stub was
introduced.

The Haiku implementation provides
`WebCore::Icon::create(RefPtr<NativeImage>&&)` with native factory/painting
tests. The extension adapter uses `BitmapImage`, scripting-disabled
`SVGImage`, `ImageBuffer`, and that factory. Decoder integration has compiled
but has not run as an extension. `PlatformHaiku.cmake` includes the native
adapter. Screen-scale selection uses the current native page's one backing
pixel per CSS pixel.

| Translation unit | Native compile result |
| --- | --- |
| `WebExtension.cpp` | Passed, 23.39 seconds |
| `WebExtensionMatchPattern.cpp` | Passed, 18.31 seconds |
| `WebExtensionLocalization.cpp` | Passed, 18.12 seconds |
| `WebExtensionPermission.cpp` | Passed, 18.76 seconds |
| `WebExtensionUtilities.cpp` | Passed, 23.70 seconds |
| `WebExtensionResources.cpp` | Passed, 20.14 seconds |
| `haiku/WebExtensionHaiku.cpp` | Passed, 24.54 seconds |

There were no diagnostics in the recorded seven-unit compile. `nm` confirms
actual `parseManifest`, `matchesURL`, `resourceDataForPath`, `recordError`,
`bestIcon`, `iconForPath`, native constructor and `availableScreenScales`
definitions. Object hashes and every copied source/header hash are retained
in the report.

The [27-check native test](../tests/EngineExtensionResourcePathTests.cpp)
executes the exact production helper on real files and symlinks. It covers
ordinary resources, a leading resource-root slash, contained dot segments,
encoded filenames, Unicode, literal percent escapes, spaces, hashes,
backslashes, control characters and trailing spaces. Negative cases include
sibling prefixes, encoded traversal, remote file authorities, non-file URLs,
raw/encoded NUL, missing resources, dangling symlinks, loops and file/directory
symlink escapes. Allowed symlinks return the checked canonical spelling;
a symlink alias for the base directory retains its boundary.

This exposed a pinned WTF URL factory behavior: a native filename containing
literal `%25` is decoded as `%` by `URL::fileURLWithFileSystemPath`. The native
adapter now percent-encodes UTF-8 bytes before URL parsing. It treats failed
`realpath` as failure; WTF's generic `FileSystem::realPath` returns unchecked
input on failure. These tests establish static canonical containment, not
atomic protection against concurrent filesystem replacement between checking
and opening a path. Immutable installed resources or descriptor-based loading
will be needed if that threat is part of the installation model.

Run current production compilation and path tests with:

```sh
bash tools/test-engine-extension-manifest-core-in-vm.sh
bash tools/test-engine-extension-resource-paths-in-vm.sh
```

Both wrappers share a probe lock and add at most one native compiler. They
copy isolated inputs under `/boot/home/summit`, read Modern target flags,
discard its disabled-feature PCH and invoke neither CMake nor Ninja. Only a
copied `cmakeconfig.h` changes `ENABLE_WK_WEB_EXTENSIONS` from 0 to 1. Run
probes after engine sync/reconfiguration completes. The native source
manifest, Ninja file, prefix and configuration remained unchanged during
both final runs.

Pinned upstream: `00991b6cdc59937da6076c69a22ae6a9460a4445`.
Native source/configuration patch used by these recorded probes:
`0be2ae56f4fe8b4e35990fddab16d604bc52ff41791e57a73815b5e41f76226e`.
The isolated report separately records later cached adapter edits. Original
configuration SHA-256:
`0917aa412d86a93afc2f97267ec4f25d7cfc320ae72ecb5780573ca98f8adc14`.

Path execution links only frozen JSC/WTF plus native system libraries. The
ICU 78 bundle is `/boot/home/summit/jsc-icu78-w8dm77f9/lib`, with JSC SHA-256
`e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba`.
The runner verifies all four frozen libraries before/after and rejects any
WebCore/WebKit dependency in the executable.

The next manifest target must link actual common/native objects and genuine
compatible Modern dependencies. `API::Object` calls real
`WebKit::InitializeWebKit2`, initializing JSC, the main thread, WebCore common
atom strings and JIT operations. API errors contain Modern Curl
`WebCore::ResourceError`; WebKitLegacy is not ABI-compatible here. The core
also references WebCore URL patterns, public suffixes, data/text decoders,
locale, MIME types, image decoders and graphics. During these probes,
`Modern/lib` had no `libWebKit`, and `libWebCore.a` was a thin archive
referencing an active build's objects. These are historical link conditions,
not a statement about the latest engine build. Copying a thin archive alone
does not freeze its dependencies. No runtime test used mutable WebCore
objects or WebKitLegacy.

Once a compatible Modern library/object bundle is stable, add upstream
manifest/match-pattern assertions using the existing in-memory constructor
and production directory adapter. The scheme-registration unit must also
compile with genuine native context/process types for the full runtime.
ZIP/XPI extraction is implemented and tested separately. Installation trust
and updates, resource-scheme serving, controller/page attachment, background
execution, content scripts and remaining C++ API bindings still need the work
in [the integration investigation](../docs/webextensions-native-integration.md).
This advances shared Safari/Chrome/Firefox WebExtensions; macOS Safari
native App Extensions remain a separate binary integration model.

Raw evidence: `.vm/extension-native-resource-core-final.log`,
`.vm/extension-native-resource-core-final-results.json`,
`.vm/extension-resource-path-tests-final.log`,
`.vm/extension-resource-path-tests-final-results.json`, and
`.vm/extension-native-resource-defined-symbols.txt`. Guest `results.json`
files in each isolated output directory are replaced by later runs.
