# Haiku media engine IPC validation

The native enum already contains `MediaPlayerMediaEngineIdentifier::Haiku`,
but its IPC serialization definition omitted that entry. This produced an
unhandled-enum warning in the full build and made the generated validator
reject Haiku's identifier. The definition now includes `Haiku` in the same
position as the C++ enum.

`tools/test-engine-media-engine-identifier.py` runs the production serializer
generator, extracts its exact validator specialization and compiles it on
Haiku against the actual `MediaPlayerEnums.h` and WTF headers. The native
harness checks all 256 byte values against an independent list of the 12
supported engines.

- Corrected definition: `.vm/media-engine-identifier-tests.fnhwi0IR/result.json`,
  256 checks pass, normal exit 0.
- Original definition: `.vm/media-engine-identifier-tests.bAs5i0ea/result.json`,
  exactly one failed check, native wire value 6 (`Haiku`), normal exit 1.
  This is the expected negative result and reproduces the compiler warning.
- Both runs retain unchanged input/source/library hashes and have no fresh
  debugger event. The runner's subsequent provenance-label change does not
  change compilation or assertions.

This verifies the generated validator function. It does not exercise IPC
transport, media playback or the full serializer translation unit. The
running full engine build is frozen on an earlier patch and has not yet
compiled this correction.

Promoted patch: `2ea935a16cbd980a09e5bc20229863c257a22e09e2fc48c29afcc8837d7936b6`.
Probe baseline: `c711e3e700a24caf8c796eee55989f36d12d139e3129f92aeea80fd5d3db26cb`.
