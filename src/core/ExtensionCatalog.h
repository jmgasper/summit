#pragma once
#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace summit {
struct InstalledExtension {
    std::string identifier;
    std::string name;
    std::string version;
    std::string fingerprint;
    std::string package;
    bool enabled = true;
    bool allowFileURLs = false;
    bool allowPrivateBrowsing = false;
    uint64_t installationOrder = 0;
};

// An import owns a private copy until Install commits it to the catalog. Pass
// Path() to WebKit's preparation API, then obtain consent for that fingerprint.
// Imports do not parse manifests, authenticate packages, or grant permissions.
class StagedExtensionPackage final {
public:
    ~StagedExtensionPackage();
    StagedExtensionPackage(const StagedExtensionPackage&) = delete;
    StagedExtensionPackage& operator=(const StagedExtensionPackage&) = delete;
    const std::filesystem::path& Path() const { return fPath; }
private:
    friend class ExtensionCatalog;
    explicit StagedExtensionPackage(std::filesystem::path directory);
    std::filesystem::path fDirectory, fPath;
    bool fInstalled = false;
};

// Filesystem operations are synchronous; run imports on a worker and serialize
// catalog mutations. The root is the browser profile's dedicated Extensions
// directory. Runtime grants remain in WebKit's fingerprint-bound saved state.
class ExtensionCatalog final {
public:
    explicit ExtensionCatalog(std::filesystem::path root);
    bool Load(std::vector<InstalledExtension>&, std::string& error) const;
    static uint64_t NextInstallationOrder(const std::vector<InstalledExtension>&);
    std::unique_ptr<StagedExtensionPackage> Stage(const std::filesystem::path& source, std::string& error) const;
    bool Install(StagedExtensionPackage&, InstalledExtension, std::string& error) const;
    bool SetEnabled(const std::string& identifier, bool enabled, std::string& error) const;
    // Forget only the installation record. Unload first; package/data removal
    // is a separate operation so a failed metadata write cannot lose resources.
    bool Forget(const std::string& identifier, std::string& error) const;
    std::filesystem::path PackagePath(const InstalledExtension&) const;
private:
    bool Save(const std::vector<InstalledExtension>&, std::string& error) const;
    std::filesystem::path fRoot;
};
}
