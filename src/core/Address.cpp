#include "Address.h"
#include <algorithm>
#include <cctype>

namespace summit {
std::string Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

std::string PercentEncode(std::string_view text)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

std::string EscapeHTML(std::string_view text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

Address ResolveAddress(std::string_view input)
{
    const auto text = Trim(input);
    if (text.empty() || text == "summit:home") return {"summit:home", {}, false};
    if (text.size() > 65536) return {{}, "This address is too long.", false};
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return {{}, "An address cannot contain control characters.", false};
    const auto colon = text.find(':');
    const auto slash = text.find_first_of("/?#");
    auto host = text.substr(0, slash);
    const bool local = host == "localhost" || host.rfind("localhost:", 0) == 0 || (!host.empty() && host.front() == '[');
    // A numeric port after a hostname must not be mistaken for a URI scheme.
    bool port = false;
    if (colon != std::string::npos && colon < host.size() && colon + 1 < host.size())
        port = std::all_of(host.begin() + colon + 1, host.end(), [](unsigned char c) { return std::isdigit(c); });
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash) && !port && !local) {
        auto scheme = text.substr(0, colon);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
        if (scheme == "http" || scheme == "https" || scheme == "file")
            return {scheme + text.substr(colon), {}, false};
        if (text == "about:blank") return {text, {}, false};
        return {{}, "This address type is not supported: " + scheme, false};
    }
    if (text.front() == '/') return {"file://" + text, {}, false};
    const bool whitespace = text.find_first_of(" \t\r\n") != std::string::npos;
    const bool domain = host.find('.') != std::string::npos && host.front() != '.'
        && host.back() != '.' && host.find('@') == std::string::npos;
    if (!whitespace && (local || domain)) return {(local ? "http://" : "https://") + text, {}, false};
    return {"https://duckduckgo.com/?q=" + PercentEncode(text), {}, true};
}
}
