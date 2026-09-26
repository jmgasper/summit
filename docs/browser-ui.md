# Browser chrome: Home, bookmarks bar, History and Bookmarks tabs, favicons

(Multiple windows, context menus and the Haiku look came later; see
[browser-windows.md](browser-windows.md).)

Added 26 September 2026. The sidebar (Start Page / Bookmarks / History
buttons and a list) is gone; each of its jobs moved somewhere else.

## What the user sees

- **Home button** (toolbar, after Back and Forward; also View › Home,
  Shift+Alt+H, and History › Home) opens the home page. Edit › Preferences…
  sets it: type an address, or use *Use Current Page* / *Use Start Page*.
  Empty means the built-in start page. The value is kept as typed and resolved
  like the address bar when Home is pressed, so a non-address searches.
- **Bookmarks bar** under the toolbar, as in Safari's Favorites bar. It shows
  bookmarks flagged for the bar, with their icons. Click opens in the current
  tab; middle-click or Alt-click opens a new tab; the secondary button offers
  Open, Open in New Tab, Remove from Bookmarks Bar and Delete Bookmark.
  Bookmarks that do not fit go in a » menu. View › Hide/Show Bookmarks Bar
  (Shift+Alt+B) or Preferences toggle it.
- **Star button** (toolbar) is filled when the current page is bookmarked. It
  offers Add to Bookmarks Bar / Add to Bookmarks, or Remove from Bookmarks Bar
  / Remove Bookmark. The Bookmarks menu has the same commands (Alt+D,
  Shift+Alt+D) and lists up to 40 bookmarks.
- **Bookmarks › Show All Bookmarks** (Alt+B) opens a *Bookmarks* tab: the bar's
  bookmarks, then the others, each with its icon, with a search field. Clicking
  one opens it in that same tab; Back returns to the list.
- **History › Show History** (Alt+Y) opens a *History* tab grouped by day —
  Today, Yesterday, then e.g. "Tuesday, 22 September 2026" — with each page's
  icon, address and time, and a search field. Entries saved before visits were
  timed are grouped under "Earlier". The History menu lists the 15 most recent
  pages and has Clear History….
- **Favicons** appear on tabs, the bookmarks bar and both built-in pages. A
  loading tab keeps its icon with a dot on its corner.

There is one History tab and one Bookmarks tab: the menu commands select an
open one instead of opening another. A built-in page is rewritten when it is
opened, reloaded, selected after something changed, or returned to with Back
or Forward. It does not reload itself while you are looking at it (that would
lose the scroll position and search text); Reload brings it up to date.

## How it works

- `src/core/Profile.*` — history entries carry `visited` (seconds since the
  epoch), bookmarks carry `bar`, and the profile has `homeURL` and
  `showBookmarksBar`. All are optional in `profile.json`, so older profiles
  load unchanged and older builds ignore them. `Visit` only records http(s)
  pages and says whether it did.
- `src/core/InternalPages.*` — day grouping (local calendar days, stepped back
  at noon so daylight saving cannot skip one) and the two pages' HTML. The pages
  are self-contained: icons are `data:` PNGs in a `<style>` block, one class per
  distinct icon; hrefs are only emitted for http, https and file URLs.
- `summit:history` and `summit:bookmarks` are real addresses (`ResolveAddress`),
  restored with the session like `summit:home`. The window writes them to
  `<profile>/Pages/{history,bookmarks}.html` and loads the file;
  `BrowserWindow::StoredURL` maps the file URL back, so the address bar stays
  empty and nothing reaches history.
- `src/core/Favicon.*` — `.ico` entry choice (closest to 32 px) and DIB
  decoding with the AND mask, area-averaged scaling to 16×16, a stored-deflate
  PNG encoder (no zlib), base64 and the cache key (the host). Other formats go
  through the Translation Kit in `src/ui/FaviconCache.*`, which keeps one PNG per
  host in `<profile>/Favicons/` and falls back between `www.host` and `host`.
- Engine: WebCore already collects `<link rel=icon>` candidates (and
  `/favicon.ico` when a page names none), but the Haiku port never attached an
  icon-loading client, so none were loaded. `IconLoadingClientHaiku` in
  `Source/WebKit/UIProcess/haiku/WebView.cpp` accepts the non-SVG favicon
  closest to 32 px (Haiku has no SVG translator) and posts
  `B_WEBKIT_ICON_LOADED` ('wico': `view`, `url`, `iconURL`, `mimeType`, `data`,
  at most 1 MiB) to the view's notification target. The engine fetches it with
  the page's own network session, cookies and cache.
- `summitctl --team ID sidebar` now hides the bookmarks bar so benchmark
  viewports stay full height (`hide-bookmarks-bar` is the same command).

## Tests

`build-host/summit_tests` covers the profile fields, address resolution, ICO
selection and decoding (1- and 32-bit, masks), scaling, PNG output, day
grouping (with `TZ=UTC`) and HTML escaping. A PNG from `EncodePNG` was also
checked with PIL.

On the workstation (bundle `bundle-14gx1mrh`, PGO engine with the icon client)
everything above was driven through VNC with `tools/bench/vnc-input.py`, which
can now use other buttons, hold clicks (Haiku menus ignore 100 ms clicks from
VNC) and type text. Two bugs were found and fixed that way: a History tab that
reloaded itself forever (its own load counted as a history change), and a
refresh that cancelled a click on a History entry.

## Not done

- No bookmark editing (rename, reorder, folders) beyond add/remove and
  bar membership; no drag and drop onto the bar.
- History keeps one entry per address (its latest visit), as before.
- SVG-only favicons are not shown.
- Menus list pages without icons.

## Installed

The workstation desktop launcher `/boot/home/Desktop/Summit-current.sh` points
to `bundle-14gx1mrh` (the `SkiaCGMiPGO` engine rebuilt with the icon client, so
performance matches `bundle-av2hgf30`). The previous launcher is
`Summit-current.pre-browser-ui-20260926.sh`. Instances already running from
other bundles keep the old interface until they are restarted.
