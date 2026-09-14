# Native content-rule storage and mapped cache data

The feature-enabled WebKit link exposed two platform gaps: the content-rule
store's default path and `NetworkCache::Data::tryCreateSharedMemory()`.
The native store now uses the existing application-specific settings-path
helper and a `ContentRuleLists` directory. Its actual adapter compiles;
creating or using the content-rule store has not yet been tested at runtime.

The Curl cache adapter now retains the original file handle when adopting
a mapped file on Haiku. Copies of the cache data share that ownership. It
wraps mapped bytes using the native shared-memory implementation, which
owns a duplicate descriptor and retains a read-only export capability for
read-only files. The memory address remains borrowed from the cache data;
an exported handle creates an independent mapping for its receiver.

Evidence:

- `.vm/cache-mapping-tests.vK9QIZhW/result.json`: all 17 native checks pass.
  These cover absent/buffer-backed data, exact mapped bytes, copied-owner
  lifetime, read-only handles and VM protection, unlinking, independent
  receiver lifetime, and preserving a borrowed mapping after descriptor
  duplication fails. The native store adapter also compiles in this run.
- `.vm/cache-mapping-tests.PaHrJ72W/result.json`: all 39 existing native
  shared-memory checks pass against the changed implementation, including
  child-process descriptor transfer, copy-on-write and app-server bitmap
  sharing. Both runs exit normally with unchanged input/source/library
  hashes and no fresh debugger event.
- Tests link the actual cache/shared-memory units with frozen
  JavaScriptCore/WTF and private ICU, without a WebCore or WebKit library.
  `tools/test-engine-cache-mapping.py` reproduces the focused checks;
  `--shared-memory-regression` selects the existing regression suite.

Earlier probes are retained: `Cm2AjdUN` and `UVon3vRk` failed during
compilation on staging/include and type issues; `vWE58kMn` compiled the
units but failed to link because unused shared-buffer functions were not
discarded. The runner now compiles functions into separate sections.
`okOfWera` ran the 17 checks and failed the closed-descriptor case, exposing
the initial use of a borrowed descriptor where a duplicate was required.
That implementation was corrected before the two passing runs above.

Content-rule compilation/loading, transport through real WebKit IPC and DNR
execution remain unverified. The running full engine build is frozen on the
earlier `1b839dea...` patch and does not contain this correction.

Promoted patch: `55777c374a27e62895e7404aaea3efe33042b78fe1c7d4d459fe4bc057faf861`.
Probe baseline: `1b839dea86a02bbebcf8210d82f7ba85c51c7b1fea0421f09ebea8622f254f27`.
