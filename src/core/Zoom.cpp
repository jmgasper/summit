#include "Zoom.h"
#include <cmath>
#include <iterator>

namespace summit {
static constexpr double kLevels[] = {0.3, 0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.2, 1.33, 1.5, 1.7, 2.0, 2.4, 3.0, 4.0, 5.0};

double NextZoomLevel(double zoom, int direction)
{
    if (!std::isfinite(zoom)) zoom = 1;
    if (direction > 0) {
        for (double level : kLevels) if (level > zoom + 0.005) return level;
        return std::end(kLevels)[-1];
    }
    if (direction < 0) {
        for (auto level = std::rbegin(kLevels); level != std::rend(kLevels); ++level)
            if (*level < zoom - 0.005) return *level;
        return kLevels[0];
    }
    return zoom;
}

bool IsDefaultZoom(double zoom)
{
    return std::fabs(zoom - 1) < 0.005;
}

std::string ZoomLabel(double zoom)
{
    return std::to_string(std::lround(zoom * 100)) + "%";
}

std::string ZoomKey(std::string_view url)
{
    if (url.rfind("summit:", 0) == 0) return std::string(url);
    if (url.rfind("file:", 0) == 0) return "file";
    size_t start;
    if (url.rfind("https://", 0) == 0) start = 8;
    else if (url.rfind("http://", 0) == 0) start = 7;
    else return { };
    auto authority = url.substr(start, url.find_first_of("/?#", start) - start);
    if (auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    std::string key;
    for (char c : authority) key += c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    if (key.rfind("www.", 0) == 0) key.erase(0, 4);
    if (key.empty() || key.size() > 255) return { };
    return key;
}
}
