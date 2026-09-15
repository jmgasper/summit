#include "core/ExtensionCatalog.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::cerr << __LINE__ << ": " #value "\n"; } } while (0)
static void put(const fs::path& path, const std::string& value)
{
    std::ofstream output(path, std::ios::binary);
    output << value;
}
static std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
int main()
{
    using namespace summit;
    char temporary[] = "/tmp/summit-extension-catalog-XXXXXX";
    if (!mkdtemp(temporary)) return 2;
    const fs::path root(temporary), storage = root / "Extensions", source = root / "source.xpi";
    ExtensionCatalog catalog(storage);
    std::string error;
    std::vector<InstalledExtension> entries;
    CHECK(catalog.Load(entries, error) && entries.empty() && error.empty());
    CHECK(!fs::exists(storage));
    put(source, "owned archive bytes");
    fs::path abandoned;
    {
        auto staged = catalog.Stage(source, error);
        CHECK(staged && error.empty());
        if (!staged) return 2;
        abandoned = staged->Path().parent_path();
        CHECK(read(staged->Path()) == "owned archive bytes");
        CHECK(catalog.Load(entries, error) && entries.empty());
    }
    CHECK(!fs::exists(abandoned));
    auto staged = catalog.Stage(source, error);
    CHECK(staged && error.empty());
    if (!staged) return 2;
    put(source, "changed external bytes");
    CHECK(read(staged->Path()) == "owned archive bytes");
    fs::remove(source);
    CHECK(fs::exists(staged->Path()));
    InstalledExtension entry { "fixture@example.test", "Native extension — 测试", "1.2.3", std::string(64, 'a'), "", true, true, false };
    auto malformed = entry;
    malformed.fingerprint = "unapproved";
    CHECK(!catalog.Install(*staged, malformed, error) && !error.empty());
    malformed = entry;
    malformed.identifier = "../escape";
    CHECK(!catalog.Install(*staged, malformed, error));
    CHECK(!fs::exists(storage / "catalog.json") && fs::exists(staged->Path()));
    CHECK(catalog.Install(*staged, entry, error) && error.empty());
    CHECK(!catalog.Install(*staged, entry, error));
    auto installedPath = staged->Path();
    staged.reset();
    CHECK(fs::exists(installedPath));
    CHECK(catalog.Load(entries, error) && entries.size() == 1);
    if (entries.size() != 1) return 2;
    CHECK(entries[0].identifier == entry.identifier && entries[0].name == entry.name && entries[0].version == entry.version);
    CHECK(entries[0].enabled && entries[0].allowFileURLs && !entries[0].allowPrivateBrowsing);
    CHECK(entries[0].fingerprint == entry.fingerprint && catalog.PackagePath(entries[0]) == installedPath);
    struct stat status { };
    CHECK(!::stat((storage / "catalog.json").c_str(), &status) && (status.st_mode & 0777) == 0600);
    CHECK(!::stat(installedPath.c_str(), &status) && (status.st_mode & 0777) == 0600);
    CHECK(!::stat(installedPath.parent_path().c_str(), &status) && (status.st_mode & 0777) == 0700);
    CHECK(catalog.SetEnabled(entry.identifier, false, error));
    CHECK(catalog.Load(entries, error) && !entries[0].enabled);
    auto saved = read(storage / "catalog.json");
    CHECK(!catalog.SetEnabled("missing", true, error) && read(storage / "catalog.json") == saved);
    put(source, "second archive");
    {
        auto duplicate = catalog.Stage(source, error);
        CHECK(duplicate && !catalog.Install(*duplicate, entry, error));
        CHECK(read(storage / "catalog.json") == saved && fs::exists(installedPath));
        ExtensionCatalog other(root / "Other");
        CHECK(duplicate && !other.Install(*duplicate, entry, error));
        CHECK(!fs::exists(root / "Other/catalog.json"));
        auto invalidText = entry;
        invalidText.identifier = "another@example.test";
        invalidText.name = std::string(1, static_cast<char>(0xff));
        CHECK(duplicate && !catalog.Install(*duplicate, invalidText, error));
        CHECK(read(storage / "catalog.json") == saved && fs::exists(duplicate->Path()));
    }
    {
        ExtensionCatalog uppercaseCatalog(root / "Uppercase");
        auto uppercaseStage = uppercaseCatalog.Stage(source, error);
        auto uppercase = entry;
        uppercase.fingerprint = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
        auto invalidDigest = uppercase;
        invalidDigest.fingerprint[0] = 'G';
        CHECK(uppercaseStage && !uppercaseCatalog.Install(*uppercaseStage, invalidDigest, error));
        CHECK(uppercaseStage && uppercaseCatalog.Install(*uppercaseStage, uppercase, error));
        std::vector<InstalledExtension> restored;
        CHECK(uppercaseCatalog.Load(restored, error) && restored.size() == 1);
        CHECK(restored.size() == 1 && restored[0].fingerprint == uppercase.fingerprint);
    }
    auto directory = root / "directory";
    fs::create_directories(directory / "nested");
    put(directory / "manifest.json", "{\"manifest_version\":2}");
    put(directory / "nested/code.js", "const retained = true;");
    fs::create_symlink("nested/code.js", directory / "alias.js");
    {
        auto unpacked = catalog.Stage(directory, error);
        CHECK(unpacked && read(unpacked->Path() / "nested/code.js") == "const retained = true;");
        CHECK(unpacked && fs::read_symlink(unpacked->Path() / "alias.js") == "nested/code.js");
        CHECK(unpacked && read(unpacked->Path() / "manifest.json") == "{\"manifest_version\":2}");
    }
    fs::create_symlink(source, root / "linked.xpi");
    CHECK(!catalog.Stage(root / "linked.xpi", error));
    CHECK(!catalog.Stage(root / "missing", error));
    CHECK(!catalog.Stage(root, error));
    CHECK(::mkfifo((directory / "pipe").c_str(), 0600) == 0);
    CHECK(!catalog.Stage(directory, error));
    size_t packages = 0;
    for (const auto& ignored : fs::directory_iterator(storage / "packages")) { (void)ignored; ++packages; }
    CHECK(packages == 1); // Failed and abandoned imports all cleaned up.
    { std::ofstream sparse(source); sparse.seekp(128 * 1024 * 1024); sparse.put('x'); }
    CHECK(!catalog.Stage(source, error));
    put(storage / "catalog.json", "{broken");
    CHECK(!catalog.Load(entries, error) && !error.empty() && entries.size() == 1);
    CHECK(!catalog.SetEnabled(entry.identifier, true, error) && read(storage / "catalog.json") == "{broken");
    auto invalid = nlohmann::json::parse(saved);
    invalid["extensions"][0]["package"] = "../../external";
    put(storage / "catalog.json", invalid.dump());
    CHECK(!catalog.Load(entries, error));
    InstalledExtension unsafe = entry;
    unsafe.package = "../../external";
    CHECK(catalog.PackagePath(unsafe).empty());
    invalid = nlohmann::json::parse(saved);
    invalid["extensions"].push_back(invalid["extensions"][0]);
    put(storage / "catalog.json", invalid.dump());
    CHECK(!catalog.Load(entries, error));
    put(storage / "catalog.json", saved);
    CHECK(catalog.Forget(entry.identifier, error));
    CHECK(catalog.Load(entries, error) && entries.empty());
    CHECK(fs::exists(installedPath)); // Metadata removal does not delete package or runtime data.
    CHECK(!catalog.Forget(entry.identifier, error));
    fs::remove(storage / "catalog.json");
    fs::create_symlink(root / "missing-catalog", storage / "catalog.json");
    CHECK(!catalog.Load(entries, error));
    fs::remove(storage / "catalog.json");
    { std::ofstream sparse(storage / "catalog.json"); sparse.seekp(4 * 1024 * 1024); sparse.put('x'); }
    CHECK(!catalog.Load(entries, error));
    fs::remove_all(root);
    CHECK(!fs::exists(root));
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
