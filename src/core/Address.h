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
std::string EscapeHTML(std::string_view text);
Address ResolveAddress(std::string_view input);
}
