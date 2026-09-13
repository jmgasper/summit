Post-relink WTF validation
=========================

Run these after the Modern build finishes and while its binaries are unchanged.
They do not configure, synchronize, or build the engine. The missing-symbol input
is the preserved 127-symbol link failure; the matcher selects its 115 WTF symbols.

`tests/fixtures/modern-link-missing-symbols.json` preserves exactly the original
127 symbol names, in their original order, from
`.vm/modern-font-view-link-missing.json`; diagnostic occurrence counts are omitted.
The failed Modern build used WebKit source
`00991b6cdc59937da6076c69a22ae6a9460a4445` and Haiku patch
`0be2ae56f4fe8b4e35990fddab16d604bc52ff41791e57a73815b5e41f76226e`.
The original ignored JSON has SHA256
`dbf6ba5830737ff13c825f9029547b5ece4d381f2f66465bc935d0615fc20a93`.
The checked-in fixture makes the export check independent of that local evidence.

Capture actual dynamic definitions and binary hashes:

```sh
mkdir -p .vm
bash tools/haiku.sh 'nm -D -C --defined-only /boot/home/summit-webkit/WebKitBuild/Modern/lib/libJavaScriptCore.so.18.7.4' > .vm/modern-jsc-postlink.exports
bash tools/haiku.sh 'nm -D -C --defined-only /boot/home/summit-webkit/WebKitBuild/Modern/lib/libWebKit.so.1.10.0' > .vm/modern-webkit-postlink.exports
bash tools/haiku.sh 'sha256sum /boot/home/summit-webkit/WebKitBuild/Modern/bin/jsc /boot/home/summit-webkit/WebKitBuild/Modern/lib/libJavaScriptCore.so.18.7.4 /boot/home/summit-webkit/WebKitBuild/Modern/lib/libWebKit.so.1.10.0' > .vm/modern-postlink-binaries.before.sha256
python3 tools/check-wtf-exports.py --missing tests/fixtures/modern-link-missing-symbols.json --jsc-exports .vm/modern-jsc-postlink.exports --webkit-exports .vm/modern-webkit-postlink.exports --output .vm/modern-wtf-postlink-results.json
```

Expected result: **115/115** definitions exported by JavaScriptCore, with no
matching definitions in WebKit. The helper also checks ownership sentinels for
main-thread initialization, the main RunLoop, memory pressure, and `g_config`.
This is a focused ownership check; it does not prove that every possible global
or inline definition appears only once. An export-only run can omit
`--webkit-exports`, and its report explicitly marks ownership unchecked.

The existing `tests/EngineSmoke.js` supplies **31 checks**: 16 with JIT/WebAssembly,
then 15 in interpreter-only mode. `tools/test-jsc-in-vm.sh` targets `Release` and
sets `DISABLE_ASLR=1`; use the following explicit Modern commands for this gate:

```sh
tar -cf - tests/EngineSmoke.js | bash tools/haiku.sh 'mkdir -p /boot/home/summit/tests && tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'readelf -d /boot/home/summit-webkit/WebKitBuild/Modern/bin/jsc' > .vm/modern-jsc-postlink.dynamic
bash tools/haiku.sh 'cd /boot/home/summit-webkit/WebKitBuild/Modern && env -u DISABLE_ASLR LIBRARY_PATH=/boot/home/summit-webkit/WebKitBuild/Modern/lib:/boot/home/summit-deps/icu78/lib:/boot/system/lib ./bin/jsc /boot/home/summit/tests/EngineSmoke.js && env -u DISABLE_ASLR LIBRARY_PATH=/boot/home/summit-webkit/WebKitBuild/Modern/lib:/boot/home/summit-deps/icu78/lib:/boot/system/lib ./bin/jsc --useJIT=false -e "globalThis.summitJavaScriptOnly = true" /boot/home/summit/tests/EngineSmoke.js' > .vm/modern-jsc-postlink-smoke.log 2>&1
bash tools/haiku.sh 'sha256sum /boot/home/summit-webkit/WebKitBuild/Modern/bin/jsc /boot/home/summit-webkit/WebKitBuild/Modern/lib/libJavaScriptCore.so.18.7.4 /boot/home/summit-webkit/WebKitBuild/Modern/lib/libWebKit.so.1.10.0' > .vm/modern-postlink-binaries.after.sha256
cmp .vm/modern-postlink-binaries.before.sha256 .vm/modern-postlink-binaries.after.sha256
```

Check the recorded JSC RPATH points to `Modern/lib`, both shell invocations exit
zero, and the log contains `16 checks, 0 failures` and `15 checks, 0 failures`.
The explicit library search path selects the Modern library and its private ICU
78 dependencies. Preserve the hashes and logs with the final engine evidence.
This gate does not run Test262 or establish browser/extension runtime success.

When the original local pre-relink JSC export capture is available, it supplies
a negative control; that ignored capture is not required for post-relink checks:

```sh
python3 tools/check-wtf-exports.py --missing tests/fixtures/modern-link-missing-symbols.json --jsc-exports .vm/modern-jsc-dynamic-defined.txt --output .vm/modern-wtf-export-negative-control.json
```

That command must exit **1**, reporting **0/115** expected exports. A positive
post-relink result remains pending until the corrected JavaScriptCore and
WebKit shared libraries are linked.
