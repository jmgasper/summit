#pragma once
#include <string>
#include <string_view>

namespace summit {
// Page zoom, as in Firefox: fixed steps from 30% to 500%, remembered per site.
constexpr double kMinimumZoom = 0.3;
constexpr double kMaximumZoom = 5.0;
// The next step above (direction > 0) or below (direction < 0) zoom; the
// limit itself when there is none.
double NextZoomLevel(double zoom, int direction);
// True when zoom is (close enough to) 100%.
bool IsDefaultZoom(double zoom);
// "110%".
std::string ZoomLabel(double zoom);
// The site a page's zoom is remembered for: the lowercase host of an http(s)
// address without "www." (and with its port), "file" for local files, the
// address itself for Summit's own pages, and empty for anything else.
std::string ZoomKey(std::string_view url);
}
