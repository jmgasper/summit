#include "config.h"
#include "WebExtensionArchiveHaiku.h"
#include "WebExtensionResourcePathsHaiku.h"

#include <Application.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <zip.h>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>

static unsigned checks, failures;
static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}

static String nativePath(const std::filesystem::path& path) { return String::fromUTF8(path.c_str()); }
static std::string read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-archive-tests");
    WTF::initializeMainThread();
    std::printf("ARCHIVE_LIBRARY %s\n", zip_libzip_version());
    char directory[] = "/tmp/summit-extension-archives-XXXXXX";
    if (!mkdtemp(directory))
        return 2;
    const auto root = std::filesystem::path(directory);
    const auto fixtures = std::filesystem::path(argv[1]);
    const auto output = root / "extracted %25 雪";
    std::filesystem::create_directories(output);
    { std::ofstream outside(root / "outside.txt"); outside << "unchanged outside resource"; }
    using WebKit::WebExtensionArchiveLimitsHaiku;
    auto extract = [&](const char* name, const WebExtensionArchiveLimitsHaiku& limits = { }) {
        return WebKit::extractWebExtensionArchiveHaiku(nativePath(fixtures / name), limits, nativePath(output));
    };
    auto emptyOutput = [&] { return std::filesystem::is_empty(output); };

    for (const char* name : { "valid.zip", "valid.xpi", "zip64.zip" }) {
        String extractedPath;
        {
            auto result = extract(name);
            check(result.has_value(), name);
            if (!result)
                continue;
            extractedPath = result->path();
            auto path = std::filesystem::path(extractedPath.utf8().legacyCStringPointer());
            check(std::filesystem::is_regular_file(path / "manifest.json"), "manifest stays at the archive resource root");
            struct stat rootStatus { }, manifestStatus { };
            stat(path.c_str(), &rootStatus);
            stat((path / "manifest.json").c_str(), &manifestStatus);
            check((rootStatus.st_mode & 0777) == 0700 && (manifestStatus.st_mode & 07777) == 0600,
                "private directory and resource permissions ignore archive privilege bits");
            check(manifestStatus.st_nlink == 1, "resources are independent regular files, never hard links");
            if (std::string(name) != "zip64.zip") {
                check(read(path / "_locales/en/messages.json") == "{\"name\":{\"message\":\"Native archive\"}}", "locale resource bytes are preserved");
                auto icon = read(path / "icons/icon.png");
                check(icon.size() == 68 && icon.substr(1, 3) == "PNG", "binary icon bytes are preserved");
                check(read(path / "asset %25 # 雪.txt") == "exact Unicode resource", "Unicode and percent filenames retain their identity");
                check(std::filesystem::is_directory(path / "empty"), "explicit empty directories remain usable");
                auto base = WebKit::fileURLForWebExtensionPathHaiku(nativePath(path / ""));
                check(!WebKit::resolveWebExtensionResourceURLHaiku(base, "icons/icon.png"_s).isEmpty(), "extracted resources use the production contained resolver");
            }
        }
        check(!FileSystem::fileExists(extractedPath) && emptyOutput(), "unconsumed archive directory is removed on destruction");
    }

    for (const char* name : { "parent.zip", "nested-parent.zip", "absolute.zip", "windows-drive.zip", "unc.zip", "backslash.zip", "dot.zip", "empty-component.zip", "colon.zip", "symlink.zip", "symlink-child.zip", "fifo.zip", "socket.zip", "character.zip", "block.zip", "duplicate.zip", "directory-file.zip", "directory-data.zip", "long-name.zip", "truncated.zip", "crc.zip", "encrypted.zip", "empty.zip", "missing-manifest.zip", "wrapped-manifest.zip", "crx-wrapper.crx", "self-extracting.zip", "not-zip.zip", "hardlink.tar", "nul-collision.zip" }) {
        auto result = extract(name);
        check(!result, name);
        check(emptyOutput(), "failed archive leaves no extracted directory");
    }
    {
        auto result = extract("nul.zip");
        check(result.has_value(), "libzip's NUL-to-space filename normalization remains contained");
        if (result) {
            auto path = std::filesystem::path(result->path().utf8().legacyCStringPointer());
            check(read(path / "name tail") == "nul" && !std::filesystem::exists(path / "name"),
                "embedded NUL cannot truncate a resource filename");
        }
    }
    check(emptyOutput(), "normalized archive resources retain automatic cleanup");
    auto limited = [&](const char* name, WebExtensionArchiveLimitsHaiku limits, const char* description) {
        auto result = extract(name, limits);
        check(!result && result.error() == WebKit::WebExtensionArchiveErrorHaiku::ExpansionLimit, description);
        check(emptyOutput(), "expansion-limit failure removes partial resources");
    };
    WebExtensionArchiveLimitsHaiku limits;
    limits.archiveBytes = 1;
    limited("small.zip", limits, "compressed input size is bounded");
    limits = { }; limits.fileBytes = 32;
    limited("small.zip", limits, "per-file expanded size is bounded");
    limits = { }; limits.expandedBytes = 65;
    limited("small.zip", limits, "combined expanded size is bounded");
    limits = { }; limits.entries = 1;
    limited("small.zip", limits, "archive entry count is bounded");
    limits = { }; limits.entries = 3;
    limited("deep.zip", limits, "implicit directory creation also obeys the entry bound");
    limits = { }; limits.pathDepth = 3;
    limited("deep.zip", limits, "resource path depth is bounded");
    limits = { }; limits.pathBytes = 20;
    limited("path-limit.zip", limits, "resource path length is bounded");
    limits = { }; limits.fileBytes = 128 * 1024;
    limited("bomb.zip", limits, "highly compressed expansion is rejected before writing its body");

    static constexpr char invalidParent[] = "/tmp\0ignored";
    auto invalidTemporaryParent = WebKit::extractWebExtensionArchiveHaiku(nativePath(fixtures / "small.zip"), { }, String::fromUTF8(std::span { invalidParent, sizeof(invalidParent) - 1 }));
    check(!invalidTemporaryParent, "temporary parent NUL cannot redirect extraction");
    std::filesystem::create_symlink(fixtures / "small.zip", root / "archive-alias");
    auto linkedInput = WebKit::extractWebExtensionArchiveHaiku(nativePath(root / "archive-alias"), { }, nativePath(output));
    check(!linkedInput && emptyOutput(), "archive helper does not follow a replaced source symlink");

    String movedPath;
    {
        auto result = extract("small.zip");
        if (!result)
            return 2;
        movedPath = result->path();
        auto moved = WTF::move(result.value());
        check(result->path().isEmpty() && FileSystem::fileExists(moved.path()), "moving directory ownership clears the previous owner");
    }
    check(!FileSystem::fileExists(movedPath), "moved ownership removes resources exactly once");
    String releasedPath;
    {
        auto result = extract("small.zip");
        if (!result)
            return 2;
        releasedPath = result->release();
    }
    check(FileSystem::fileExists(releasedPath), "release transfers ownership for the existing WebExtension destructor");
    FileSystem::deleteNonEmptyDirectory(releasedPath);
    check(emptyOutput(), "transferred resources are removable by the existing cleanup operation");
    check(read(root / "outside.txt") == "unchanged outside resource", "outside files remain untouched across every archive case");
    std::filesystem::remove_all(root);
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
