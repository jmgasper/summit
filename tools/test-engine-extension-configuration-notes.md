Native controller configuration, 2026-09-14
=========================================

The actual `WebExtensionControllerConfiguration` common implementation, new
Haiku adapter, and real-class assertion source compile with
`ENABLE_WK_WEB_EXTENSIONS=1` in a private native output directory. This is
**two production units and one test unit compiled, with no runtime assertions
executed**. The shared engine remains configured with extensions disabled.

Run the isolated compile with:

```sh
bash tools/test-engine-extension-configuration-in-vm.sh
```

The wrapper copies cached production sources and headers, verifies their hashes,
and uses the configured Modern compiler flags without its feature-disabled PCH.
Only a copied `cmakeconfig.h` enables extensions. Compiles run sequentially under
the shared extension-probe lock; the runner does not invoke CMake or Ninja or
write native engine sources. It reuses the manifest-core compile runner without
altering that runner's original source list or reports.

Implemented behavior
--------------------

The existing common default, nonpersistent, temporary, and UUID constructors
remain the factories. The new native methods supply their missing storage-path
and copy implementation:

- Persistent paths use the same `StoragePathsHaiku` application profile root as
  native website data stores:
  `B_USER_SETTINGS_DIRECTORY/WebKit/<encoded application signature>/WebExtensions/Default`,
  or an uppercase UUID in place of `Default`. Computing the path does not create
  a persistent directory; upstream controller loading creates storage later.
- Temporary configurations call the common `FileSystem::createTemporaryDirectory`
  helper. The companion common helper fix joins the parent and filename and
  passes a NUL-terminated mutable string to `mkdtemp`. Its independent filesystem
  tests are described by `test-engine-temporary-directories.py`; this configuration
  compile does not execute or retest that helper.
- Copying preserves the UUID, temporary flag, exact storage path, and referenced
  website data store. It creates neither an unused temporary directory nor an
  unused ephemeral data store. Custom paths set through the existing setter
  survive copying; explicit browser-profile/controller wiring is still required.
- Equality is now available in the common implementation on native platforms.
  Cocoa retains its existing web-view configuration comparison. Native equality
  compares the UUID, path, and stored website data store pointer, following the
  existing upstream field semantics.

Temporary directories retain upstream lifetime: the configuration has no
directory-deleting destructor, and copied or serialized configurations can share
the path. Native copying does not introduce deletion that could invalidate another
configuration. The prepared tests explicitly clean their directories after use.

Evidence and remaining link requirement
---------------------------------------

`test-engine-extension-configuration-results.json` records all three successful
objects, source/object hashes, undefined symbols, and unchanged native input
snapshots. The configured native source manifest reports patch
`0be2ae56f4fe8b4e35990fddab16d604bc52ff41791e57a73815b5e41f76226e`.
The copied candidate source hashes, rather than that older native patch identity,
identify the new code compiled by this probe. Pinned upstream is
`00991b6cdc59937da6076c69a22ae6a9460a4445`.

`EngineExtensionConfigurationTests.cpp` contains real-class cases adapted from
upstream's `WKWebExtensionControllerConfiguration.mm`: factory persistence and
UUIDs, default/ephemeral website data store identity, lazy persistent paths,
custom paths, copy/equality, and temporary directory permissions and shared
lifetime. The source compiled; none of these assertions ran.

At this probe, `WebKitBuild/Modern/lib` contained no `libWebKit.so*`. The common
object requires real `API::Object::Object`,
`WebsiteDataStore::createNonPersistent`, and `WebsiteDataStore::defaultDataStore`.
`API::Object` in turn calls actual `InitializeWebKit2`, including WebCore
initialization. Execution needs a compatible frozen Modern engine and the fixed
common temporary helper, with any private link dependencies supplied by the real
engine. No initialization stubs or Legacy/Curl ABI substitutions were used.

This configuration step does not load a controller, create background pages,
inject content scripts, or run an installed extension.
