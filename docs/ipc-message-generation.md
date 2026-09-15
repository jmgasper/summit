# Conditional IPC metadata and incremental generation

Cookie observer registration and removal use priority dispatch on Haiku/Curl.
Their complementary declarations retain ordinary dispatch for other platform
configurations. The previous generator combined duplicate message names into one
enumerator but reads dispatch attributes only from the first declaration. This
incorrectly makes the native priority flags apply to the other configurations.

The correction emits conditional description rows when the declarations
have different dispatch flags. It keeps the existing shared row when their
flags agree. The message identifiers and generated headers remain unchanged.
It is integrated in engine patch `32b97d3a...`, after the cookie-ordering build
and all three queued native runtime checks passed.

The same correction changes the generator entry point to write UTF-8 output only
when its bytes differ. Previously, even an unchanged header received a new
timestamp, causing dependent compilation after a metadata-only edit. The native
CMake generation rule has `restat = 1`, which lets Ninja retain dependents when
their generated inputs remain unchanged. The actual native rebuild completed in
seven steps: generation, one C++ compilation, the library and helper links, and
the library symlinks. Only `MessageNames.cpp` changed among 377 native generated
outputs; the other 376 retained both bytes and timestamps. Ninja then reported
no pending work for WebKit, WebProcess or NetworkProcess.

The host regression tool runs the real parser/model/generator and preprocesses
the generated C++ under seven configurations. It checks the actual cookie
declarations, bounded and unbounded dispatch permissions, and asynchronous
reply metadata. It also exercises the generator entry point with repeated
inputs, changed metadata and a changed source list, and runs WebKit's existing
generated-file expectations for 31 receiver fixtures.

```sh
python3 tools/test-engine-ipc-metadata.py --report .vm/conditional-ipc-metadata-check.json
```

The tool checks the prepared engine cache; `--overlay` selects a candidate. At patch
`b42d85ea...`, that baseline has seven failing cases: four conditional-metadata
cases and three timestamp checks. The candidate passes all nine test methods,
including the existing generated-file tests, with the same harness, parser and
model inputs. The integrated cache also passes all nine methods. These are host
generation checks, not native runtime results.

A separate replay uses all 186 receiver inputs from the actual native Ninja
generation rule. Its baseline matches the native `MessageNames.h` and
`MessageNames.cpp` hashes. Of 378 generated outputs, only `MessageNames.cpp`
changes, and only the two cookie-observer description entries differ. Repeating
the candidate generation preserves every output's bytes and timestamp.

Evidence is preserved in `.vm/conditional-ipc-metadata-final-baseline.json`,
`.vm/conditional-ipc-metadata-final-candidate.json`,
`.vm/conditional-ipc-production-check.json`,
`.vm/conditional-ipc-full-output-check.json`, and
`.vm/conditional-ipc-native-restat.json`. Integrated verification is recorded in
`.vm/conditional-ipc-metadata-promoted.json`,
`.vm/modern-extensions-conditional-ipc-metadata-build-result.json`, and
`.vm/conditional-ipc-native-incremental-check.json`. The host replay includes a
separately requested `ReceiverSources.txt`, explaining its 378 outputs versus
377 in the native generation directory.
