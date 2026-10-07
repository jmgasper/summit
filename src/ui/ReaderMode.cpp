#include "ReaderMode.h"
#include <Application.h>
#include <File.h>
#include <Resources.h>
#include <Roster.h>
#include <array>
#include <fstream>

namespace summit {
namespace {
std::string Resource(int32 identifier)
{
    // Use a private resource reader: toolbar images may be loaded on other
    // window threads while these scripts are initialized.
    app_info info;
    if (!be_app || be_app->GetAppInfo(&info) != B_OK) return {};
    BFile file(&info.ref, B_READ_ONLY);
    BResources resources(&file);
    size_t size = 0;
    auto* bytes = static_cast<const char*>(resources.LoadResource('RAWT', identifier, &size));
    return bytes && size && size < 500000 ? std::string(bytes, size) : std::string();
}
}
const std::string& ReaderProbeScript()
{
    static const auto source = [] {
        auto library = Resource(601);
        return library.empty() ? std::string() : "(() => {\n" + library + R"(
if (!['http:', 'https:'].includes(location.protocol)
    || !['text/html', 'application/xhtml+xml'].includes(document.contentType)
    || document.getElementsByTagName('*').length > 50000) return false;
return isProbablyReaderable(document);
})())";
    }();
    return source;
}
const std::string& ReaderExtractScript()
{
    static const auto source = [] {
        auto readability = Resource(602), purify = Resource(603), extract = Resource(604);
        return readability.empty() || purify.empty() || extract.empty() ? std::string()
            : readability + "\n;\n" + purify + "\n;\n" + extract;
    }();
    return source;
}
std::string NewReaderURL()
{
    std::array<unsigned char, 24> bytes;
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return {};
    const char* hex = "0123456789abcdef";
    std::string url = "summit-reader://";
    for (auto byte : bytes) { url += hex[byte >> 4]; url += hex[byte & 15]; }
    return url + "/";
}
}
