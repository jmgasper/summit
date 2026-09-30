# Default browser, extensions in private windows, pinned extensions

Added 30 September 2026, with the developer tools' Storage tab
([developer-tools.md](developer-tools.md)) and BWebView on Summit's engine
([legacy-webview.md](legacy-webview.md)).

## What the user sees

### Default browser (Preferences › General)

The first line of the Preferences window says which browser web links open in
("Summit is your default browser.", or "Your default browser is
WebPositive.") and, when it is not Summit, offers **Make Summit the Default
Browser**. The system then opens web links (`http`, `https`) and web pages
(`text/html`, XHTML) in Summit: links from other applications, `open URL` in
Terminal, HTML files double-clicked in Tracker. A running Summit opens them in
a new tab of its front normal window; otherwise Summit starts.

To go back, choose another application for those types in FileTypes (or use
the other browser's own setting).

### Extensions in private windows (Extensions window)

Each extension has **Allow in private windows** (off by default). Allowed,
the extension works in private windows as in normal ones: its content scripts
and rules apply to their pages, it sees their tabs and windows (as
`incognito`), and its toolbar button is in their toolbar too. Not allowed, it
knows nothing of private windows: no content, no tabs, no window or tab
events, no button. The change applies at once, to pages already open.

It is one extension, not a second copy: its background page, its saved data
(1Password's vault) and its state are the same in both kinds of window, as in
Firefox and in Chrome's default mode. The pages of private windows keep their
own cookies and storage, forgotten when the last private window closes.

### Pinned extensions (toolbar and Extensions window)

A secondary click on an extension's toolbar button opens a menu: **Unpin from
Toolbar** and **Manage extensions…**. An unpinned extension leaves the
toolbar; its action is in the menu of the toolbar's extensions button (`…`),
which appears when any action has no button (as before for more than four).
**Show on the toolbar** in the Extensions window pins it back. Pinning is
remembered (`unpinnedExtensions` in `profile.json`) and applies to every
window.

## How it works

- **Default browser** (`src/ui/DefaultBrowser.*`). `MakeDefaultBrowser()`
  sets Summit's signature as the preferred application of
  `application/x-vnd.Be.URL.http`, `…https`, `text/html` and
  `application/xhtml+xml`, and the signature's application hint. Two things
  shaped the hint:
  - The roster forgets a hint whose file does not carry the application's
    signature, and then *queries* the disks for any file with it (it found a
    stray test script). So the hint names a Summit executable, which has the
    signature in its resources.
  - A launcher script cannot be the target either: the roster hands the files
    Tracker opens to the launched program as a `B_REFS_RECEIVED` message, not
    as arguments, and a script never sees it.

  The executable is therefore started by the system directly, without its
  launcher's environment (its GPU libraries, the engine's switches). Summit
  now reads that environment from `launch.env` beside the executable before
  anything else (`ApplyLaunchEnvironment()`): `NAME=VALUE` sets a variable
  that is not set, `NAME^=VALUE` puts a path in front of `LIBRARY_PATH`. The
  engine's processes inherit it. `tools/install-on-workstation.sh` writes
  `launch.env` into the installed bundle and keeps a link
  `/boot/home/summit/Summit-installed` to its executable; the desktop
  launcher exports `SUMMIT_SYSTEM_LAUNCHER` with that link, which is what the
  hint names, so it follows each install. A Summit started otherwise names its
  own executable. At startup, the installed build, when it is the default,
  re-points a hint that names something else to its link
  (`RefreshDefaultBrowserHint()`, on a thread of its own). The name shown in
  Preferences comes from the preferred application's hint; Summit does not
  call `BRoster::FindApp()`, which has the registrar create MIME entries as
  it resolves (the X399's SSH stopped answering right after a test Summit
  made that call at startup; the cause was not established).
- **Private windows and extensions.** Each `BWebKitContext` had its own
  extension controller, so the private context's pages were controlled by an
  empty one. A private context is now made from the normal one
  (`BWebKitContext(extensionHost)`, `WebViewContextHaiku::createPrivate()`):
  its own non-persistent website data store and process pool, but the normal
  context's extension controller, tab registry and extension packages.
  WebKit's controller already separates private pages (they get the
  *private* user content controllers, which only extensions with
  `hasAccessToPrivateData` fill) and `WebExtensionTab::isPrivate()` already
  follows the page's session. What was added: a window is private when its
  pages are (`WebExtensionWindow::isPrivate()`), window and tab events of
  private windows go only to extensions allowed there (and a hidden window
  taking the focus leaves the others' last focused window alone), and
  `BWebKitContext::SetExtensionPrivateAccess()` changes an extension's access
  at once. The Extensions window's switch saves `allow_private_browsing` in
  the extension catalog and calls it; at the next start the extension loads
  with it. Browser windows show extension buttons in private windows too; the
  engine lists only the allowed extensions' actions there.
- **Pinning.** `Profile::unpinnedExtensions`; the window orders its actions
  pinned first and gives buttons to at most four of those; the rest go in the
  extensions menu. A secondary click on `ExtensionActionButton` posts
  `kExtensionActionMenu` to its window. The application changes the profile
  (`kExtensionSetPinned`) and every window hears
  `SharedProfile::kExtensionsPinnedChanged`.

## Tests

- `build-host/summit_tests` (104 checks: the profile's `unpinnedExtensions`
  saved and read, absent in old profiles) and
  `build-host/summit_extension_catalog_tests` (101 checks:
  `SetAllowPrivateBrowsing()` saved, and refused for an extension that is not
  installed, leaving the catalog as it was).
- `tools/bench/private-probe/`: a test extension (MV2) with a content script
  that marks the page and reports to one background page, which logs every
  page it saw with `incognito` and a running count, and tab and window
  creation; a popup that says whether its tab is private.
- `summitctl`: `installext PACKAGE` (installs through the Extensions window,
  approving it), `privateext ID on|off`, `pin ID on|off`, `defaultbrowser`,
  `makedefault`.

On the X399 with a private copy of `bundle-xfr_wr8d`/`bundle-wkujmujj` and a
fresh profile:

| | |
| --- | --- |
| Not allowed | a private window and a private tab: no content script, no events (no *tab created*, no *window created*); the probe's button absent from the private toolbar. (The first build still sent the two events; fixed in the engine.) |
| Allowed | the content script ran at once in the private pages already open (`incognito=true`), a new private tab gave *tab created incognito=true*, and the count went on from the normal window's (1, 2, 3...): one background page. The button appeared in the private toolbar and its popup said "Private tab: …". |
| Withdrawn | after reloading, the private page had no content script and the button was gone; the normal window unaffected |
| Pinning | the button's menu (Unpin from Toolbar, Manage extensions…), unpinned: the button replaced by the extensions menu holding it, `unpinnedExtensions` saved; the Extensions window showed *Show on the toolbar* off and *Allow in private windows* on; ticking it put the button back |
| Default browser | before: WebPositive for all four types. After Make Default: Summit for all four and the hint set; `open http://…` opened a tab in the running test Summit. An HTML file from `open FILE` reached a launcher script without its path, which is why the hint now names the executable (see above); that version is built but was not yet tried on the X399 when this was written. The owner's settings were to be put back after the test. |
