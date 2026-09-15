# Copying a frozen modern bundle

After a modern browser bundle has completed and its runtime checks have passed,
run this on the host from the Summit checkout:

```sh
python3 tools/copy-modern-browser-bundle.py
# A saved report may be selected explicitly:
python3 tools/copy-modern-browser-bundle.py .vm/modern-browser-bundle.json
# The separate preview bundle is also supported:
python3 tools/copy-modern-browser-bundle.py --preview
# An extension-enabled browser has a separate report:
python3 tools/copy-modern-browser-bundle.py .vm/modern-extensions-browser-bundle.json
```

The default report is `.vm/modern-browser-bundle.json`; `--preview` selects
`.vm/modern-preview-bundle.json`. The tool uses the existing VM SSH connection
in `tools/haiku.sh`. It does not build, synchronize, prepare, or launch anything.
It requires Python 3.9 or newer, Git, and enough host disk space for the native
transport archive, extracted bundle and compressed engine source.

The result is a new `artifacts/modern-browser/bundle-<native ID>` directory.
An existing directory, file or symlink at that destination causes a failure.
Extraction and verification happen in a private temporary directory; a failed
copy removes only that temporary directory. Completed bundles are never replaced.

The native path and manifest must match the selected host report. Every declared
binary, private library, launcher and frozen app input is checked by SHA-256.
Only declared library aliases pointing to their regular library in `lib/` are
accepted; archive traversal, hard links, special files and undeclared source
files are rejected. Summit and WebKit licenses are checked against their matching
source; the bundled private ICU license is preserved and its digest recorded.
Extension-enabled bundles also contain the locked private libzip library and
its complete license notice from the locked `zip.h` header. Their manifest
records the selected engine variant and requires both extension CMake options
to be ON; ordinary modern bundles require both options OFF. Older manifests
remain readable with their original configuration and header mapping.

The cached `.cache/WebKit` must already have the bundled upstream commit and
patch. The exact `git diff --binary --full-index HEAD` digest must equal the
bundled engine lock, and the lock's patch file must have that same digest.
Untracked and ignored source files are rejected except `__pycache__` entries,
which native source synchronization also excludes. Git flags that hide source
changes are rejected. For Git-mandated CRLF files, the normalized blob is also
checked against the index; the archive retains and hashes the actual CRLF bytes. The copier never repairs or reapplies a patch. A mismatch
requires selecting a genuinely matching checkout/bundle before trying again.

`WebKit-sources.tar.xz` contains the complete native build input trees
`CMakeLists.txt`, `Configurations`, `Source` and `Tools`, with the applied patch.
It preserves source symlinks within those trees. It excludes Git metadata,
generated builds and Python caches. This is the same source scope transferred
by `tools/build-webkit-in-vm.sh`, rather than a complete upstream Git clone.
`WebKit-source-manifest.json` records every archived file, link and directory.
All source bytes and the locks/build support are fingerprinted before and after
archiving; changes abort publication. Bytes sent to the source archive are also
checked while writing it.

The original `build-manifest.json` and `host-bundle-report.json` are preserved.
`copy-provenance.json` records their digests, the source archive digest, matching
before/after fingerprints and a file inventory of the final payload excluding
the provenance file itself. The final command output reports the provenance
digest. These hashes establish the copied inputs; they do not establish browser
runtime behavior. The current verified copy is `bundle-yqhmejfw`, including the
native process fixes, basic download UI, navigation errors and history handling.
Earlier artifacts are preserved with their recorded limitations.
See [the verification record](STATUS.md) for exact runtime results and remaining
limitations.

## Building from the preserved inputs

The copied bundle runs natively on Haiku with `./run-browser.sh` or
`./run-preview.sh`. Keep the helpers and private libraries beside it. Runtime
results must come from that frozen bundle, with its matching helpers and ICU.

`source/` is the exact app source and public headers frozen by the native build.
`rebuild/` contains the engine/ICU locks, complete port patch and host build
scripts captured when copying. Its `tools/build-modern-browser.py` comes from
the frozen app inputs. The other host scripts have separate provenance and are
not represented as original native compilation inputs.

To set up a fresh build workspace, copy the two trees into an empty directory:

```sh
mkdir summit-rebuild
cp -a source/. summit-rebuild/
cp -a rebuild/. summit-rebuild/
cp summit-rebuild/LICENSE-Summit summit-rebuild/LICENSE
cd summit-rebuild
```

Configure your own VM access in `.vm/` for `tools/haiku.sh` (Haiku SSH on host
port 2225, user `user`, key `id_ed25519`, known hosts file `known_hosts`). No SSH
credentials are distributed. The native VM needs the original compiler and
development dependencies; the native manifest records the compiler and original
commands. Use a separate VM or ensure no existing build is active, because the
build scripts use `/boot/home/summit-webkit` and `/boot/home/summit-deps/icu78`.

```sh
# Downloads and verifies ICU 78.3 using engine/icu.lock.json, then builds/tests it.
bash tools/build-icu-in-vm.sh
# Fetches the locked upstream commit and applies the preserved patch in a new cache.
python3 tools/prepare-webkit.py
# Builds the modern engine and both process helpers with the private ICU.
SUMMIT_WEBKIT_JOBS=8 bash tools/build-webkit-in-vm.sh --modern
# Compile and freeze the app corresponding to the preserved source tree:
bash tools/build-modern-browser-in-vm.sh --browser
# For a preview source bundle, use the following command instead:
# bash tools/build-modern-browser-in-vm.sh
```

For an extension-enabled bundle, use `--modern-extensions` on both the engine
build and app bundler instead:

```sh
SUMMIT_WEBKIT_JOBS=3 bash tools/build-webkit-in-vm.sh --modern-extensions
bash tools/build-modern-browser-in-vm.sh --browser --modern-extensions
```

This uses `/boot/home/summit-webkit-extensions`, preserves the private libzip
dependency, and writes `.vm/modern-extensions-browser-bundle.json` without
replacing the ordinary modern report. The bundler checks that both process
targets are fully rebuilt before copying them. `--compile-only` can compile the
browser against isolated staged headers while an engine build is running; it
does not link, bundle, or establish runtime behavior. Extension-enabled browser
runtime verification is still pending; see [STATUS.md](STATUS.md).

The source archive also supplies those full patched trees for a direct native
CMake build without fetching Git. It is already patched: do not apply the patch
again. It has no `.git`, so extracting it into `.cache/WebKit` does not create
the Git checkout expected by `prepare-webkit.py`. Consult the preserved
`tools/build-webkit-in-vm.sh` for the exact native CMake configuration and paths.
Rebuilding is separate from copying and does not promise identical binary hashes
or substitute for testing the rebuilt browser.
