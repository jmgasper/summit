Native ZIP/XPI extension resource loading, 2026-09-14

The production archive extractor passes **111 native checks** against real
ZIP/XPI fixtures and libzip 1.11.4. The updated native WebExtension constructor
also compiles with `WK_WEB_EXTENSIONS=1`, following the recorded **7-unit
production core compile and 27 resource-path checks**. The full-engine build
keeps extensions **OFF**; these isolated probes do not run an extension.
The constructor's actual manifest execution still requires the compatible
Modern WebCore/WebKit link described in [the resource-port notes](test-engine-extension-manifest-core-notes.md).

The [archive test report](test-engine-extension-archives-results.json) retains
source, fixture, object, executable and dependency hashes. The separate
[constructor report](test-engine-extension-archives-constructor-results.json)
records its native compile and unchanged configuration/source snapshots.

This bounded change adds:

- `Source/WebKit/UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.{h,cpp}`:
  an actual ZIP reader and a move-only temporary-directory owner.
- `Source/WebKit/UIProcess/Extensions/haiku/WebExtensionHaiku.cpp`: accept
  regular ZIP/XPI files through the existing native constructor, preserving
  directory loading. Parse the actual upstream manifest before releasing
  temporary-directory ownership into `m_resourcesAreTemporary`.
- `Source/WebKit/UIProcess/Extensions/WebExtension.h`: rename the native
  constructor parameter to `resourcePath` to reflect both accepted inputs.
- `tests/EngineExtensionArchiveTests.cpp` and the archive fixture/native
  runner tools. The existing core wrapper now snapshots the archive header
  needed by the constructor.

The accompanying `PlatformHaiku.cmake` change includes the native archive
source and, only when `ENABLE_WK_WEB_EXTENSIONS` is enabled, requires
`libzip>=1.11` through `PkgConfig::LIBZIP`. The normal
feature-disabled build does not require libzip. Shared GLib/Cocoa resource
handling remains unchanged by the native archive adapter.

The extractor uses `zip_fdopen` with `ZIP_RDONLY | ZIP_CHECKCONS`, strict
filename decoding, original ZIP external file attributes, real CRC-checked
`zip_fread`, and real filesystem writes. It accepts stored/deflated ZIP,
including ZIP64, with a regular `manifest.json` at the archive root. It
rejects encrypted archives, corrupt/truncated data, unsupported compression,
CRX/self-extracting prefixes and non-ZIP formats. It does not implement CRX,
Safari native app binaries, signature/trust verification or installation UI.

All output lives beneath a newly created private 0700 directory. Components
are traversed with `openat`/`mkdirat`, `O_DIRECTORY`, `O_NOFOLLOW` and close-on-
exec descriptors. Files use `O_EXCL | O_NOFOLLOW` with mode 0600; archive
permissions, ownership, ACLs and executable/set-ID bits are never applied.
Absolute paths, dot/parent components, backslashes, drive/stream colons,
duplicate names, directory/file collisions, symlinks, FIFOs, devices and
sockets are rejected. No hard-link creation operation is used. ZIP has no
standard hard-link object: a real tar hard-link fixture is rejected at the
format boundary, and successful resources are verified to have link count 1.

Default limits are 128 MiB input, 64 MiB per expanded file, 256 MiB total expanded
bytes, 10,000 archive entries and independently 10,000 created filesystem
nodes, 1024 relative path bytes, 255 bytes per component and 32 components.
Sizes are checked before writing and again while streaming; directories
created implicitly also consume the node limit. Tests use smaller explicit
limits to exercise every bound without consuming those production maxima.

A failed extraction deletes its entire private directory. An unconsumed
successful extraction also deletes it when its owner is destroyed. The
native constructor keeps that owner until upstream manifest parsing
succeeds, so parse failure cleans up immediately. Successful construction
transfers ownership to WebExtension's existing temporary-resource destructor.
The native tests execute move/release/cleanup behavior with real files;
the updated WebExtension constructor and upstream destructor were compiled
in isolated probes. The archive test executable does not instantiate
WebExtension or execute manifest parsing.

The 111 checks cover stored ZIP, deflated XPI, ZIP64, manifest/locales/icon
bytes, Unicode/percent filenames, explicit and implicit directories,
permissions, links and special entries, traversal, duplicate and conflicting
entries, malformed CRC/encryption/truncation, every expansion bound,
unconsumed/moved/released directory ownership and untouched outside files.

Source inspection found two relevant library behaviors. Libarchive 3.7.9's
ZIP reader deliberately rewrites FIFO metadata to regular files, preventing
strict rejection of original special entries through its public API; this
implementation therefore uses libzip's original external attributes.
Libzip 1.11.4 maps impossible native NUL filename bytes to spaces in
`lib/zip_io_util.c::_zip_read_data`. Tests verify that this does not truncate
filenames or escape containment, and reject collisions produced by that
normalization. No second archive metadata parser was added to override the
library's behavior.

The configured HaikuPorts repository supplied these privately unpacked
packages; neither was installed into the OS:

| Package | Repository SHA-256 |
| --- | --- |
| `libzip-1.11.4-1-x86_64.hpkg` | `f22023b112ed362bce11fef7b3c36f995c9ec4c46e644ae889dabb916bd567ec` |
| `libzip_devel-1.11.4-1-x86_64.hpkg` | `b90fefccc5b81c5947339a3367bf3d63943d550438f57acd8f0abd3b937e5b78` |

Package URLs use
`https://eu.hpkg.haiku-os.org/haikuports/r1beta6/x86_64/current/packages/`
plus the exact filenames above. Checksums were read from the native cached
HaikuPorts repository with `package_repo list -v`, then checked before
unpacking. The test dependency root is
`/boot/home/summit/extension-archive-dependencies`; `package extract` placed
the runtime in `libzip/` and development files in `libzip-devel/`. Raw package
files remain at that root. The library
`libzip/lib/libzip.so.5.5` has SHA-256
`0bbc6d259c41639ac68b964f42842fd0feedcd5c27fae3d11daeb85283e81fd7`.

After preparing those private dependencies, reproduce with:

```sh
bash tools/test-engine-extension-archives-in-vm.sh
bash tools/test-engine-extension-manifest-core-in-vm.sh --source haiku/WebExtensionHaiku.cpp
```

The wrapper generates independent fixtures with Python's standard ZIP/tar
libraries and copies only isolated source/test inputs. Tests link the private
libzip and frozen ICU 78 JSC/WTF bundle, plus native system libraries. The
runner rejects WebCore/WebKit dependencies, verifies all dependency/source
hashes remained unchanged, and records the configured Modern flags. Both
probes share the extension lock and run at most one compiler. They never
invoke CMake/Ninja or write to the native engine source/build directories.

Both recorded reports use native source/configuration patch
`0be2ae56f4fe8b4e35990fddab16d604bc52ff41791e57a73815b5e41f76226e`
and separately hash the tested extension source edits. This provenance
identifies these probes rather than the latest full-engine build. The
constructor compiled in 22.07 seconds without diagnostics. Raw final evidence is
`.vm/extension-archive-tests-final.log`,
`.vm/extension-archive-tests-final-results.json`,
`.vm/extension-archive-constructor-compile.log` and
`.vm/extension-archive-constructor-compile-results.json`.
