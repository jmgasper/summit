# Current upstream WebKit port

The engine target is upstream WebKit `main`, pinned on September 13, 2026:
`00991b6cdc59937da6076c69a22ae6a9460a4445` (upstream timestamp
2026-09-12 23:12:08 -0700).

The platform changes originate in Haiku's actively maintained Codeberg port:
`295ca3199ad0f8563ce664fb15597bc3a2b2e3fe` (August 30, 2026).
Its most recent merged upstream baseline is
`24545f4fcbd7c5ed088348ec64e7e039da43eb00` (August 29).
The GitHub Haiku mirror is archived; Codeberg is authoritative.

`sources.lock.json` records the commits and the SHA-256 of the complete
patch against the pinned modern upstream. The patch retains all upstream
copyright/license headers. It currently restores the Haiku platform and
legacy embedding API, adjusts preferences for the current generator schema,
and adapts the legacy Curl loader to the new cookie-storage interfaces. The
cookie adapter preserves the original database and passes native regression
checks plus the running browser's cookie/fetch fixture.

```sh
python3 tools/prepare-webkit.py
bash tools/build-webkit-in-vm.sh
# Individual build targets are also accepted:
bash tools/build-webkit-in-vm.sh jsc
bash tools/test-jsc-in-vm.sh
bash tools/test262-in-vm.sh
bash tools/test262-in-vm.sh atomics
bash tools/test262-in-vm.sh intl
# Full suite with upstream proposal/feature skips, ignoring failure expectations:
bash tools/test262-in-vm.sh full
# Focused native WebCore cookie and event-loop lifecycle checks:
bash tools/test-engine-cookies-in-vm.sh
bash tools/test-download-names-in-vm.sh
bash tools/test-engine-runloop-in-vm.sh
bash tools/test-engine-process-in-vm.sh
bash tools/test-engine-memory-in-vm.sh
bash tools/test-engine-socket-monitor-in-vm.sh
bash tools/test-engine-message-packets-in-vm.sh
bash tools/test-engine-storage-paths-in-vm.sh
bash tools/test-engine-modules-in-vm.sh
bash tools/test-engine-bitmaps-in-vm.sh
# Build Summit with private current-engine libraries and accompanying source:
bash tools/build-current-browser-in-vm.sh
# Repeat the host bundle copy without rebuilding, while inputs still match:
bash tools/copy-current-browser-bundle.sh
```

Do not run a second engine build while one is active. Inspect the live guest
`ninja` process and host SSH session, and resume that build after a terminal
failure has been diagnosed. The build tree is retained between iterations.

The separate modern process port uses private ICU 78.3. Its build entry point is
available while the remaining platform integration is developed:

```sh
bash tools/build-icu-in-vm.sh
SUMMIT_WEBKIT_JOBS=8 bash tools/build-webkit-in-vm.sh --modern
# Or select a modern target:
bash tools/build-webkit-in-vm.sh --modern WebProcess
```

Both modes check for an active engine build before synchronizing their shared
source directory. The modern configuration enables WebKit's process architecture
and currently disables the GPU process. It does not yet provide a working
browser process connection. Its ICU 78 JavaScriptCore passes 31 JIT/interpreter
smoke checks; the scoped conformance comparison fixes 94 ICU-related failures.
The full ICU 78 conformance run is still in progress.

Current status: Haiku CMake configuration succeeds with ICU 74.1. Current
JavaScriptCore and its shell build successfully after correcting the initial
ICU 66 selection. Runtime checks pass with JavaScript JIT enabled and disabled;
the normal engine also executes a WebAssembly module. WebCore and WebKitLegacy
compile and link. **Summit runs the current engine**, reporting 626.1.6 and the
exact upstream commit, with both private library paths verified in the native
loader. Its 37 native browser checks pass and WebKit.org renders over HTTPS.
Eleven of twelve advanced platform probes pass; BroadcastChannel is unavailable
because upstream disables it in the legacy embedding pending PageGroup isolation.
A selected 116-file Test262 corpus passes 232 runs without skips or failures.
The full run records 101,755 passes, 234 failures and 486 skipped files; 138
failures match pinned upstream Linux expectations by path, mode and exit code.
See the [verification record](../docs/STATUS.md) for the remaining differences.

Next port work includes the modern WebKit process architecture and extensions.
Upstream moved networking and cookie functions from
WebCore into WebKit during September; the Haiku legacy Curl backend needs
corresponding adaptation, now compiled and exercised in Summit. The adapter does not yet
address the inherited database's missing SameSite and partitioned-cookie
support. Its existing non-Curl backend is not an alternate
validated path. The unfinished Haiku multiprocess API in the patch is enabled
in the separate modern build, with full browser integration still pending.
Platform feature switches inherited from the Haiku
port (including disabled WebGL, WebAudio and media APIs) must be reviewed
against the full-browser objective.

The [extension integration design](EXTENSIONS.md) records the shared upstream
runtime, Haiku's missing process infrastructure and the compatibility evidence
needed for the original three-ecosystem requirement.

An optional private ICU 78.3 build addresses the older system ICU's Unicode
and locale data. `engine/icu.lock.json` pins its verified upstream archive;
`bash tools/build-icu-in-vm.sh` builds, installs under
`/boot/home/summit-deps/icu78`, and runs upstream tests. All four test groups and
the configuration self-check pass. Configure an engine with
`-DICU_ROOT=/boot/home/summit-deps/icu78` to select it. The existing current-browser
bundle continues to use ICU 74 until an updated engine is built and verified.

Primary source references:

- https://github.com/WebKit/WebKit
- https://codeberg.org/haiku/haikuwebkit
- https://github.com/haikuports/haikuports/blob/master/haiku-libs/haikuwebkit/haikuwebkit-1.10.0.recipe
- https://webkit.org/blog/
