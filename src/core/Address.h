#pragma once
#include <string>
#include <string_view>

namespace summit {
struct Address {
    std::string url;
    std::string error;
    bool search = false;
};
std::string Trim(std::string_view text);
std::string PercentEncode(std::string_view text);
// Convert an absolute native path to a URL, preserving directory separators.
std::string FileURL(std::string_view path);
std::string EscapeHTML(std::string_view text);
Address ResolveAddress(std::string_view input);
}
