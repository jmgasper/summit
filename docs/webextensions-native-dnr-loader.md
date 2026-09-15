# Native declarative request-rule loading

Engine patch `c3ff5b56144df26ae0b6d85df42c1ca453867d6a77f3fefa8a87966cc092519b`
integrates this loader. The preceding popup build reached the linker with this
function as its only missing symbol. Its first combined build exposed a missing
`API::ContentRuleList` forward declaration in three other users of the shared
context header. That known-failed build was stopped before linking.

Patch `c8b960d698b09f68d906606c00f4ac52e7a2d14fda74a42b2bb7ac49037fb81b`
adds the declaration; all three previously failing units compile with it. The
corrected full build is running. A combined engine link has not yet been
demonstrated.

The native loader reads session and dynamic rules from their SQLite stores,
collects enabled static rulesets, translates them off the WebKit main loop and
compiles them through the real content-rule store. This is an initial DNR
implementation, not complete Chrome, Firefox or Safari compatibility.

The current translator supports `block` and `allow`, URL/regex filters, case
sensitivity, supported explicit resource types and the default non-main-frame
resource set. Rules are ordered by priority, with `allow` winning a tie against
`block`. Each terminal rule carries its own stop condition, so a lower-priority
rule cannot override a higher-priority match. URL-filter alternatives retain
one action location through the existing WebCore pipeline.

Unsupported actions and conditions reject the entire translation. In particular,
redirects, upgrades, header changes, `allowAllRequests`, domain conditions,
request methods, resource exclusions and an explicit `other` category still
need integration. Host-access-only DNR is rejected until request-level host
permission evaluation is implemented. The initial block/allow route requires
the `declarativeNetRequest` permission.

Two existing engine details need correction before those conditions can work:
WebCore currently reads a subresource's method from the initiating document's
request, and its legacy `other` filter expands to ping and CSP-report resources.
The loader does not silently translate those fields into broader matches.
The full [Chrome DNR reference](https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest)
remains the compatibility target, alongside Firefox and Safari behavior.

Loading validates rule and ruleset IDs, JSON shapes, duplicate IDs, supported
fields and bounded source/output sizes. It does not mutate source arrays or
publish rule IDs before successful installation. A missing static resource,
failed database read, malformed rule or compilation failure preserves the
previously installed list and records an error. An empty valid result removes
the active list and its compiled file.

Every asynchronous phase checks the loaded context and its generation. Each
compilation owns a unique file path; obsolete work can only remove its own file.
The active list is retained in memory, so adding a new content controller does
not resurrect rules from a stale disk lookup. Permission revocation and unload
clear the active list and invalidate pending work. Private controller additions
recheck access. Compiled files are replaced on successful reload and removed on
normal unload; persistent cache reuse and crash-orphan cleanup remain future work.

The content-rule store's background JSON parse-error path now dispatches its
completion to the main loop, matching the successful and compiler-error paths.
The loader relies on that thread ownership. A dedicated persistent-store fixture
compiles, but its runtime check still requires the engine to link.

## Evidence and limits

The real WebCore pipeline passes **95 checks** in Haiku. It exercises translated
priority, URL boundaries, case sensitivity, resource selection, duplicate and
invalid identifiers, unsupported fields, size limits and source preservation.
The test compiles real rule bytecode and inspects actions returned by the actual
backend. It links the configured WebCore archive, with no WebKit library or
extension context. Input/library hashes remained unchanged and the native
crash-log interval contained no events.

```sh
python3 tools/test-engine-content-rule-pipeline.py --dnr
```

The recorded run used `--overlay .vm/extension-dnr-loader-candidate` before
integration. Evidence: `.vm/content-rule-pipeline.qTVYOVho/result.json`.
The promoted source audit is
`.vm/extension-dnr-loader-current-validation.json`; all ten integrated files
match the tested candidate. Full-build output is mirrored to the visible QEMU
progress Terminal. The initial build log is
`.vm/modern-extensions-dnr-loader-build.log`; the correction is being built in
`.vm/modern-extensions-dnr-declaration-build.log`. The declaration and three
successful compile checks are recorded in
`.vm/extension-dnr-declaration-current-validation.json`.

Seven affected engine translation units compile. The translator and loader pass
in `.vm/extension-lifecycle-inputs.x504Y4da/result.json`; that stage's separate
store unit failed because the older feature-disabled build lacked a generated
WebCore header. The store, context, native context, permission observer and DNR
API lifecycle unit then pass against the configured feature-enabled tree in
`.vm/extension-lifecycle-inputs.uL5cSOY7/result.json`.

The declaration correction passes `WebExtensionControllerAPITestHaiku.cpp`,
`WebExtensionControllerHaiku.cpp` and `WebViewContextHaiku.cpp` in
`.vm/extension-lifecycle-inputs.fv0lNpoH/result.json`. Both native source trees
contain all ten current DNR files. The idle feature-disabled libraries were not
rebuilt; the feature-enabled combined build remains pending.

The new `EngineContentRuleStoreTests.cpp` fixture compiles against that corrected
configuration in `.vm/content-rule-pipeline.Kb5j7cuM/result.json`. It uses the real
`API::ContentRuleListStore`, with an isolated temporary directory and application
main loop. It is intended to check asynchronous parse/compiler/write failures,
successful persistence, reopening, replacement, source recovery, deletion and
completion lifetime after the caller releases the store. **It has not run yet.**

After the full engine build completes:

```sh
python3 tools/test-engine-content-rule-pipeline.py --store \
  --resume /boot/home/summit/content-rule-pipeline.Kb5j7cuM
```

The runner refuses stale engine targets or changed compile inputs, links the
actual WebKit library and audits the native crash log. This fixture does not
instantiate an extension or make a browser request.

These tests do not execute loader persistence, privileged IPC, a browser request,
permission revocation, stale completions or an extension. The DNR JavaScript
bindings remain disabled on Haiku. Before enabling updates, fix and test the
existing transaction pipeline: a successful rollback currently reports success
for a failed update, and commit failures are only logged. Serialization of
concurrent updates, full rule semantics and real network enforcement remain
part of the browser goal.
