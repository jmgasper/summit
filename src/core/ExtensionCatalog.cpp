#include "ExtensionCatalog.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace summit {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
constexpr size_t catalogLimit = 4 * 1024 * 1024;
constexpr uint64_t archiveLimit = 128 * 1024 * 1024;
constexpr uint64_t expandedLimit = 256 * 1024 * 1024;
bool alphaNumeric(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
bool identifierValid(const std::string& value)
{
    return !value.empty() && value.size() <= 255 && value != "." && value != ".."
        && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return alphaNumeric(c) || c == '.' || c == '-' || c == '_' || c == '@' || c == '{' || c == '}';
        });
}
bool packageValid(const std::string& value)
{
    return value.size() == 10 && value.compare(0, 4, "pkg-") == 0
        && std::all_of(value.begin() + 4, value.end(), alphaNumeric);
}
bool textValid(const std::string& value)
{
    return !value.empty() && value.size() <= 4096 && value.find('\0') == std::string::npos;
}
void validate(const InstalledExtension& entry)
{
    if (!identifierValid(entry.identifier) || !textValid(entry.name) || !textValid(entry.version)
        || !packageValid(entry.package) || entry.fingerprint.size() != 64
        || !std::all_of(entry.fingerprint.begin(), entry.fingerprint.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw std::runtime_error("Invalid installed extension record");
}
void privateDirectory(const fs::path& path)
{
    if (::mkdir(path.c_str(), 0700) && errno != EEXIST)
        throw std::runtime_error(std::strerror(errno));
    if (fs::symlink_status(path).type() != fs::file_type::directory)
        throw std::runtime_error("Extension storage is not a directory");
}
class Descriptor {
public:
    explicit Descriptor(int value) : fValue(value) { }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { if (fValue >= 0) ::close(fValue); }
    int Get() const { return fValue; }
    void Close()
    {
        int value = fValue;
        fValue = -1;
        if (::close(value)) throw std::runtime_error(std::strerror(errno));
    }
private:
    int fValue;
};
void writeAll(int descriptor, const char* bytes, size_t size)
{
    while (size) {
        auto count = ::write(descriptor, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error(std::strerror(errno));
        bytes += count;
        size -= count;
    }
}
void copyFile(const fs::path& source, const fs::path& target, uint64_t limit, uint64_t& total)
{
    Descriptor input(::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status { };
    if (input.Get() < 0 || ::fstat(input.Get(), &status) || !S_ISREG(status.st_mode)
        || status.st_size < 0 || static_cast<uint64_t>(status.st_size) > limit)
        throw std::runtime_error("Extension input is not a regular file within the import size limit");
    Descriptor output(::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (output.Get() < 0) throw std::runtime_error(std::strerror(errno));
    std::array<char, 65536> buffer;
    uint64_t copied = 0;
    for (;;) {
        auto count = ::read(input.Get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error(std::strerror(errno));
        if (!count) break;
        if (static_cast<uint64_t>(count) > limit - copied
            || static_cast<uint64_t>(count) > expandedLimit - total)
            throw std::runtime_error("Extension import exceeds its size limit");
        writeAll(output.Get(), buffer.data(), count);
        copied += count;
        total += count;
    }
    if (::fsync(output.Get())) throw std::runtime_error(std::strerror(errno));
    output.Close();
}
void copyDirectory(const fs::path& source, const fs::path& target, unsigned depth, unsigned& entries, uint64_t& total)
{
    if (depth > 32) throw std::runtime_error("Extension directory nesting is too deep");
    privateDirectory(target);
    for (const auto& item : fs::directory_iterator(source)) {
        if (++entries > 10000) throw std::runtime_error("Extension contains too many files");
        auto destination = target / item.path().filename();
        auto type = item.symlink_status().type();
        if (type == fs::file_type::directory)
            copyDirectory(item.path(), destination, depth + 1, entries, total);
        else if (type == fs::file_type::regular)
            copyFile(item.path(), destination, 64 * 1024 * 1024, total);
        else if (type == fs::file_type::symlink) {
            // Preserve links without reading their targets. WebKit preparation
            // validates containment and materializes accepted package links.
            fs::create_symlink(fs::read_symlink(item.path()), destination);
        } else
            throw std::runtime_error("Extension contains an unsupported filesystem entry");
    }
}
}

StagedExtensionPackage::StagedExtensionPackage(fs::path directory)
    : fDirectory(std::move(directory)), fPath(fDirectory / "package") { }
StagedExtensionPackage::~StagedExtensionPackage()
{
    if (!fInstalled) {
        std::error_code ignored;
        fs::remove_all(fDirectory, ignored);
    }
}
ExtensionCatalog::ExtensionCatalog(fs::path root)
    : fRoot(fs::absolute(std::move(root)).lexically_normal()) { }

bool ExtensionCatalog::Load(std::vector<InstalledExtension>& result, std::string& error) const
{
    error.clear();
    try {
        auto path = fRoot / "catalog.json";
        auto type = fs::symlink_status(path).type();
        if (type == fs::file_type::not_found) { result.clear(); return true; }
        if (type != fs::file_type::regular)
            throw std::runtime_error("Invalid extension catalog file");
        Descriptor input(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat status { };
        if (input.Get() < 0 || ::fstat(input.Get(), &status) || !S_ISREG(status.st_mode)
            || status.st_size < 0 || static_cast<uint64_t>(status.st_size) > catalogLimit)
            throw std::runtime_error("Invalid extension catalog file");
        std::string bytes;
        std::array<char, 16384> buffer;
        for (;;) {
            auto count = ::read(input.Get(), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error(std::strerror(errno));
            if (!count) break;
            if (static_cast<size_t>(count) > catalogLimit - bytes.size())
                throw std::runtime_error("Extension catalog is too large");
            bytes.append(buffer.data(), count);
        }
        auto data = json::parse(bytes);
        if (data.at("format") != 1 || !data.at("extensions").is_array() || data.at("extensions").size() > 256)
            throw std::runtime_error("Unsupported extension catalog format");
        std::vector<InstalledExtension> entries;
        std::set<std::string> identities, packages;
        for (const auto& item : data.at("extensions")) {
            InstalledExtension entry { item.at("identifier"), item.at("name"), item.at("version"),
                item.at("fingerprint"), item.at("package"), item.at("enabled"),
                item.at("allow_file_urls"), item.at("allow_private_browsing") };
            validate(entry);
            if (!identities.insert(entry.identifier).second || !packages.insert(entry.package).second)
                throw std::runtime_error("Duplicate extension catalog entry");
            entries.push_back(std::move(entry));
        }
        result = std::move(entries);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ExtensionCatalog::Save(const std::vector<InstalledExtension>& entries, std::string& error) const
{
    std::string temporary;
    error.clear();
    try {
        if (entries.size() > 256) throw std::runtime_error("Too many installed extensions");
        json items = json::array();
        for (const auto& entry : entries) {
            validate(entry);
            items.push_back({{"identifier", entry.identifier}, {"name", entry.name}, {"version", entry.version},
                {"fingerprint", entry.fingerprint}, {"package", entry.package}, {"enabled", entry.enabled},
                {"allow_file_urls", entry.allowFileURLs}, {"allow_private_browsing", entry.allowPrivateBrowsing}});
        }
        auto data = json{{"format", 1}, {"extensions", std::move(items)}}.dump(2);
        if (data.size() > catalogLimit) throw std::runtime_error("Extension catalog is too large");
        privateDirectory(fRoot);
        temporary = (fRoot / "catalog-XXXXXX").string();
        Descriptor output(::mkstemp(temporary.data()));
        if (output.Get() < 0) throw std::runtime_error(std::strerror(errno));
        writeAll(output.Get(), data.data(), data.size());
        if (::fsync(output.Get())) throw std::runtime_error(std::strerror(errno));
        output.Close();
        if (::rename(temporary.c_str(), (fRoot / "catalog.json").c_str()))
            throw std::runtime_error(std::strerror(errno));
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        if (!temporary.empty()) ::unlink(temporary.c_str());
        return false;
    }
}

std::unique_ptr<StagedExtensionPackage> ExtensionCatalog::Stage(const fs::path& source, std::string& error) const
{
    error.clear();
    try {
        auto type = fs::symlink_status(source).type();
        if (type != fs::file_type::regular && type != fs::file_type::directory)
            throw std::runtime_error("Select an extension archive or directory");
        if (type == fs::file_type::directory) {
            auto relative = fs::weakly_canonical(fRoot).lexically_relative(fs::canonical(source));
            if (!relative.empty() && *relative.begin() != "..")
                throw std::runtime_error("Extension storage cannot be imported into itself");
        }
        privateDirectory(fRoot);
        privateDirectory(fRoot / "packages");
        auto directory = (fRoot / "packages/pkg-XXXXXX").string();
        if (!::mkdtemp(directory.data())) throw std::runtime_error(std::strerror(errno));
        auto staged = std::unique_ptr<StagedExtensionPackage>(new StagedExtensionPackage(directory));
        uint64_t total = 0;
        if (type == fs::file_type::regular)
            copyFile(source, staged->Path(), archiveLimit, total);
        else {
            unsigned entries = 0;
            copyDirectory(source, staged->Path(), 0, entries, total);
        }
        return staged;
    } catch (const std::exception& exception) {
        error = exception.what();
        return nullptr;
    }
}

bool ExtensionCatalog::Install(StagedExtensionPackage& staged, InstalledExtension entry, std::string& error) const
{
    error.clear();
    try {
        if (staged.fInstalled || staged.fDirectory.parent_path() != fRoot / "packages")
            throw std::runtime_error("Prepared import belongs to another catalog or is already installed");
        auto type = fs::symlink_status(staged.Path()).type();
        if (fs::symlink_status(staged.fDirectory).type() != fs::file_type::directory
            || (type != fs::file_type::regular && type != fs::file_type::directory))
            throw std::runtime_error("Prepared import no longer exists");
        entry.package = staged.fDirectory.filename().string();
        validate(entry);
        std::vector<InstalledExtension> entries;
        if (!Load(entries, error)) return false;
        if (std::any_of(entries.begin(), entries.end(), [&](const auto& old) { return old.identifier == entry.identifier; }))
            throw std::runtime_error("This extension identity is already installed");
        entries.push_back(std::move(entry));
        if (!Save(entries, error)) return false;
        staged.fInstalled = true;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ExtensionCatalog::SetEnabled(const std::string& identifier, bool enabled, std::string& error) const
{
    std::vector<InstalledExtension> entries;
    if (!Load(entries, error)) return false;
    auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.identifier == identifier; });
    if (found == entries.end()) { error = "Extension is not installed"; return false; }
    found->enabled = enabled;
    return Save(entries, error);
}
bool ExtensionCatalog::Forget(const std::string& identifier, std::string& error) const
{
    std::vector<InstalledExtension> entries;
    if (!Load(entries, error)) return false;
    auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.identifier == identifier; });
    if (found == entries.end()) { error = "Extension is not installed"; return false; }
    entries.erase(found);
    return Save(entries, error);
}
fs::path ExtensionCatalog::PackagePath(const InstalledExtension& entry) const
{
    return packageValid(entry.package) ? fRoot / "packages" / entry.package / "package" : fs::path();
}
}
