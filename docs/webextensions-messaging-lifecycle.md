# Messaging across extension reloads

The native lifecycle test installs one persistent-background MV2 extension
through Summit's folder picker and consent UI. It verifies messages between
the background and two real documents, disables the extension in the native
manager, verifies that it has unloaded, then reenables it and navigates to fresh
documents. It exercises four disable/reenable cycles.

```sh
python3 tools/test-modern-extension-messaging-lifecycle.py --bundle NATIVE_BUNDLE
```

Each main-frame and iframe handshake supplies its actual tab, frame and document
identifiers. The background uses them for document, frame, combined-target and
Chrome callback delivery. The replies verify the receiving document, payload
and sender extension identity. The original content-to-background callback must
also complete. Both documents must reach the same background instance, and each
enable must start a new instance. The runner checks normal browser exit, process
drain, the native crash interval and unchanged source, bundle and package bytes.

The initial two-cycle version on `bundle-9lg9p4yz` passes all initial messages
but fails every tested background-to-page route after reenabling. Content scripts still reach
the new background, and their original callback responses return successfully.
Evidence: `.vm/modern-extension-messaging-lifecycle-db6754be6fa18a7fc56ed20b/result.json`.
An earlier attempt failed to compile the test harness and is not runtime evidence.

The diagnostic `bundle-hpfycsdb` reproduces the problem on the second reenable
cycle. Its trace records content-script listener registration and successful
selection of the receiving process, page and document, but finds an empty
listener list in the selected script world. Evidence:
`.vm/modern-extension-messaging-lifecycle-0ce009c81ff18d2cc463f359/result.json`
and `console.log`. Both runtime failures exit normally, drain their processes,
preserve their inputs and have clean crash intervals.

The native binding lookup previously matched isolated worlds only by their
extension name. Old documents can retain an unloaded extension's world while a
new world with the same name is created on reenable. Looking up an old world can
therefore replace the context's selected world with one that has no current
listeners. The fix sends the current content-world identifier with the
extension context and requires that identifier when binding an isolated world.
It also clears the selected world if the context receives a different identifier.
These changes are limited to the Haiku port.

Engine patch `0b559e88135efb8972eab783cfeb6769b01e7d7a27ab39516438f1fecbc7cefb`
passes its full native build, including regenerated IPC serializers. The matching
`bundle-_s5lmdfe` passes all four reenable cycles and 251 native checks. Every
document, frame, combined-target and callback route succeeds in both frames at
all five stages. Evidence:
`.vm/modern-extension-messaging-lifecycle-801663c59f7b1bff050c613a/result.json`.

The same bundle passes the unmodified Dark Reader page-theme probe (66 checks),
the general messaging suite (57 JavaScript cases and 234 native checks), and the
native popup regression (342 checks). All runs exit normally with drained
processes, clean crash intervals and unchanged inputs. Their reports are listed
in `.vm/content-world-identity-runtime-validation.json`. Dark Reader's popup
controls remain stuck on the loading screen; the page-theme result does not
establish full extension compatibility.

This test covers fresh documents after manager disable/reenable. Browser restart,
history restoration, pending-message cancellation and private browsing require
separate coverage.
