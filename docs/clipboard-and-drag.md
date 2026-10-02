# Pasting images and dropping files

Added 2 October 2026 for [issue #14](https://github.com/jmgasper/summit/issues/14)
(drag and drop files into the browser) and
[issue #15](https://github.com/jmgasper/summit/issues/15) (paste, for example a
screenshot into a GitHub issue).

## What the user sees

- **Pasting an image.** An image on the clipboard pastes into pages that
  accept files from the clipboard: GitHub's issue and comment editor uploads a
  pasted screenshot and inserts its Markdown link, as in other browsers. Haiku's
  Screenshot (*Copy to clipboard*, or `screenshot -c`) and ShowImage put a
  BBitmap on the clipboard; pages receive it as a PNG file named `image.png`.
  PNG, JPEG, GIF or WebP data that an application puts on the clipboard under
  its MIME type is passed through as it is.
- **Pasting into editable content** (a mail compose box, a rich text editor)
  inserts the clipboard's HTML, or its image, or its text. Before, only plain
  text fields took a paste; editable content received nothing.
- **Copy Image** (the page context menu) puts the image on the clipboard as PNG
  data and as a BBitmap, so it pastes into pages and into Haiku applications.
- **Dropping files.** Files dragged from Tracker onto a page are offered to it
  as an HTML drag: drop zones highlight and receive the files (GitHub uploads
  an image dropped on its editor; `<input type="file">` takes a dropped file).
  A file dropped where the page takes nothing opens in the tab, as in other
  browsers.

## How it works

All in the engine; the browser needed nothing.

- **Clipboard** (`Source/WebCore/platform/haiku/PasteboardHaiku.cpp`). The
  pasteboard reads `be_clipboard` directly in the web process (it has a
  BApplication). `fileContentState()` reports an image when the clipboard holds
  image data or a BBitmap archive (`image/bitmap`, `image/x-be-bitmap`), and
  `read(PasteboardFileReader&)` hands it to the page as a file: a BBitmap is
  converted to PNG with the Translation Kit. Upstream WebCore only lets Cocoa
  pages see pasted or dropped files (`DataTransfer::allowsFileAccess()`, see
  webkit.org/b/271957); Haiku now follows the Cocoa rule (files for paste and
  for file drags, none for other drags). Only the web's own string types are
  listed in `clipboardData.types` (BBitmap archives and application-private
  fields stay hidden).
- **Rich paste.** Editor's paste on GLib-style ports asks the pasteboard for a
  web content reader, which the Haiku pasteboard never implemented, so
  editable content received nothing. On Haiku it now uses the pasteboard's
  own `documentFragment()` (HTML, else an image as an `<img>` with a blob URL,
  else plain text).
- **Drops.** `BWebKitView::MouseMoved()` sees the drag message of a drag from
  another application. A drag that carries refs becomes a WebKit drag (entered,
  updated, exited; `WasDropped()` performs it) with the files' paths
  (`viewDragHaiku` → `WebView` → `WebPageProxy::dragEntered` and the rest).
  `DragData` carries file names over IPC on Haiku as on Cocoa, `DragDataHaiku`
  answers from them, and a drag-and-drop `Pasteboard` holds the drag's files
  instead of reading the clipboard (before, a drop read the clipboard and a
  drag that started in a page wrote it). WebKit's generic path already grants
  the network process access to dropped files, which uploads need. For a
  drop the page does not take, WebCore loads the first file in the tab; the
  UI process only lets a web process navigate to a file it was given read
  access to, so `WebPageProxy::performDragOperation` now grants it (as GTK
  does) and sends the drop once the grant is in place. Before, the
  navigation was refused ("outside the sandbox") and nothing happened.

## Not done

- Drags that start in a page and leave it (dragging an image or link out to
  Tracker), and HTML drag and drop between elements of one page: the port's
  drag client does not start a system drag (`WebDragClient::startDrag` is
  empty on Haiku).
- Dropping text or links from other applications (only files are understood).

## Checking it

`tools/bench/pages/clipboard-drop.html` reports every paste and drop: the
types, items and files the page received, image sizes, and previews.
`summitctl paste` performs Edit › Paste on the current page (no keyboard
needed on the shared desktop). `tools/bench/clipimage.cpp` puts an image file
on the clipboard as a BBitmap, as Screenshot does (`screenshot -c` works too),
and `tools/bench/dragsource.cpp` opens a small window that, when pressed,
drags the given files as Tracker does; `vnc-input.py drag` from it to the page
performs a drop with pointer events only.

Checked on the X399 on 2 October 2026 with a 320×200 PNG:

- Paste into the text area: `types` `["Files"]`, `items`
  `["file:image/png"]`, one file `image.png`, `image/png`, which decodes to
  320×200 (what GitHub's editor uploads).
- Paste into editable content: the same event, and the image is inserted as an
  `<img>` with a blob URL.
- Drop of a PNG and a text file from another application onto the drop zone:
  `items` `["file:image/png", "file:text/plain"]`, both files with their names,
  types and sizes, the image decodes and the text reads back.
- The same drop on the page's heading (no drop handler) opens the PNG in the
  tab, and Back returns to the page.

`DataTransfer.items` was missing (undefined) until the engine turned on
`DataTransferItemsEnabled`, which defaults to on only for the Cocoa, GTK, WPE
and Windows ports; scripts that look at `items` before `files` (GitHub's
among them) threw.
