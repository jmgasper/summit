# Native extension engine runtime

The full Extensions-enabled engine first links at patch
`c8b960d698b09f68d906606c00f4ac52e7a2d14fda74a42b2bb7ac49037fb81b`.
The later store-cleanup patch
`284266a9d79e372f7c617cbf1e6694b6d309035c9b725b850197da34e5a49011`
also completes all engine targets. Both builds enable `WK_WEB_EXTENSIONS` and
`CONTENT_EXTENSIONS` and use the private ICU/libzip dependencies.

The real persistent rule store passes **111 checks**; see
[request-rule loading](webextensions-native-dnr-loader.md). Extension JavaScript
execution remains unproven: the first two extension runs load their native
context but do not complete background navigation.

## Actual extension fixture

`EngineExtensionRuntimeTests.cpp` constructs an isolated manifest-v2 package
directory and loads it through the native package snapshot, extension and
controller classes. The host grants the fixture's declared storage permission
explicitly. Its extension profile is temporary and persistent for the test;
its website data store is a separately owned nonpersistent store. The controller
treats its own store as the normal extension environment.

The intended two rounds queue a native test message before the JavaScript
listener exists, check the `browser` and `chrome` bindings and runtime identity,
write storage through promises and read it through callbacks, then recreate the
extension context to check persistent state. Successful completion requires the
background DOM title, ten assertion reports, two completion reports with a
matching nonce and two success reports from the native test-message IPC route.
Compilation alone does not satisfy any of those JavaScript requirements.

```sh
python3 tools/test-engine-content-rule-pipeline.py --extension-runtime
```

The runner verifies source/header hashes and completed engine targets, links the
actual WebKit library, selects the matching helper executables and records their
hashes. It checks the native crash log and all input/library/helper hashes after
execution. Output goes to a file so orphan helpers cannot keep a pipe open.
The fixture runs in a new process group inherited by the native fork/exec
launcher; forced cleanup makes the test fail. Live Haiku team IDs determine
whether the group has drained, because `killpg(group, 0)` can still succeed
after all its live teams have exited.

Native runner checks verify normal exit, an orphan child and a timeout in
`.vm/extension-runner-process-check-live-teams.json`. They test OS process
cleanup, not WebKit or an extension. During normal fixture teardown, the host
unloads the context and explicitly requests termination of its owned helper
processes. This does not test automatic background-process retirement.

## Observed failure

The first run against the initial linked engine records **six native checks
passed and one deadline failure** in
`.vm/content-rule-pipeline.NCFQATF3/runtime-result.json`. The package snapshot,
context load and storage grant succeed. Both native helper processes start,
but the background title never becomes nonempty and no JavaScript test reports
arrive. The fixture reaches its own deadline, unloads and exits; no forced
runner cleanup or debugger event occurs.

A second run adds page-state diagnostics against the store-cleanup build:
`.vm/content-rule-pipeline.cOE5dEEj/runtime-result.json`. It shows a background
page with a running process, `loading=1`, the requested extension URL as its
active URL, and an empty provisional URL. That state does not advance before
the deadline. Again there are six native passes, one failure, no JavaScript
reports, no debugger events and no remaining owned helper processes. All source,
header, library and helper hashes remain unchanged during both runs.

The next diagnostic boundary is between the UI process's pending load request
and the first provisional-load notification from WebProcess. This fixture has
not exercised storage IPC or extension test-message delivery successfully.
It does not implement browser installation UI, store distribution, signature
verification, manifest-v3 workers, action UI, DNR enforcement or complete
Safari/Chrome/Firefox compatibility. Those remain part of the browser goal.
