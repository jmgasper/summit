#include "config.h"
#include "WebExtensionResourcePathsHaiku.h"

#include <Application.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <wtf/MainThread.h>

static unsigned checks, failures;
static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}

static String nativePath(const std::filesystem::path& path)
{
    return String::fromUTF8(path.c_str());
}

static URL directoryURL(const std::filesystem::path& path)
{
    return WebKit::fileURLForWebExtensionPathHaiku(nativePath(path / ""));
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-resource-tests");
    WTF::initializeMainThread();
    char directory[] = "/tmp/summit-extension-paths-XXXXXX";
    if (!mkdtemp(directory))
        return 2;
    const auto root = std::filesystem::path(directory);
    const auto resources = root / "extension %25 雪";
    const auto sibling = root / "extension %25 雪-sibling";
    std::filesystem::create_directories(resources / "nested");
    std::filesystem::create_directories(sibling);
    { std::ofstream file(resources / "manifest.json"); file << "{}"; }
    { std::ofstream file(resources / "asset # % 雪.txt"); file << "resource"; }
    { std::ofstream file(sibling / "secret.txt"); file << "outside"; }
    const auto specialFile = resources / "path\\segment\tline\nend ";
    { std::ofstream file(specialFile); file << "escaped native name"; }
    const auto base = directoryURL(resources);
    using WebKit::canonicalWebExtensionPathHaiku;
    using WebKit::resolveWebExtensionResourceURLHaiku;
    auto resolve = [&](const String& path) { return resolveWebExtensionResourceURLHaiku(base, path); };
    auto manifest = canonicalWebExtensionPathHaiku(nativePath(resources / "manifest.json"));
    check(base.fileSystemPath() == nativePath(resources / ""), "native directory URI round-trips literal percent escapes and Unicode");
    auto specialURL = WebKit::fileURLForWebExtensionPathHaiku(nativePath(specialFile));
    check(specialURL.fileSystemPath() == nativePath(specialFile), "native filename backslashes, control characters and trailing space survive URI conversion");
    check(resolve(specialURL.string()).fileSystemPath() == canonicalWebExtensionPathHaiku(nativePath(specialFile)), "escaped native filename loads through the checked resource resolver");

    check(resolve("manifest.json"_s).fileSystemPath() == manifest, "ordinary resource resolves within its directory");
    check(resolve("/manifest.json"_s).fileSystemPath() == manifest, "one leading slash means the extension resource root");
    check(resolve("nested/../manifest.json"_s).fileSystemPath() == manifest, "contained dot segments remain usable");
    check(resolve("%6Danifest.json"_s).fileSystemPath() == manifest, "percent-encoded filename characters resolve normally");
    check(resolve("asset%20%23%20%25%20%E9%9B%AA.txt"_s).fileSystemPath()
        == canonicalWebExtensionPathHaiku(nativePath(resources / "asset # % 雪.txt")), "spaces, percent signs, hashes and Unicode retain their filesystem identity");
    check(resolve("../extension%20%2525%20%E9%9B%AA-sibling/secret.txt"_s).isEmpty(), "sibling directory with the same textual prefix is rejected");
    check(resolve("%2e%2e/extension%20%2525%20%E9%9B%AA-sibling/secret.txt"_s).isEmpty(), "encoded parent traversal is rejected");
    check(resolve(WebKit::fileURLForWebExtensionPathHaiku(nativePath(sibling / "secret.txt")).string()).isEmpty(), "absolute sibling file URL cannot bypass containment");
    check(resolve("https://example.test/manifest.json"_s).isEmpty(), "non-file resource URL is rejected");
    check(resolve("file://remote-server/tmp/resource"_s).isEmpty(), "remote file authority is rejected");
    check(resolve(""_s).isEmpty() && resolve("/"_s).isEmpty(), "empty resource names do not expose the base directory");
    check(resolve("."_s).isEmpty() && resolve(".."_s).isEmpty(), "directory self and parent are not file resources");
    check(resolve("missing.txt"_s).isEmpty(), "nonexistent resource reports failure");
    static constexpr char nulPath[] = "manifest.json\0ignored";
    check(resolve(String::fromUTF8(std::span { nulPath, sizeof(nulPath) - 1 })).isEmpty(), "embedded NUL cannot truncate a resource path");
    check(resolve("manifest.json%00ignored"_s).isEmpty(), "percent-encoded NUL cannot truncate a resource path");

    std::filesystem::create_symlink(resources / "manifest.json", resources / "inside-link");
    check(resolve("inside-link"_s).fileSystemPath() == manifest, "contained symlink resolves to the canonical checked file");
    std::filesystem::create_symlink(sibling / "secret.txt", resources / "escape-link");
    check(resolve("escape-link"_s).isEmpty(), "symlink to a sibling resource is rejected");
    std::filesystem::create_directory_symlink(sibling, resources / "escape-directory");
    check(resolve("escape-directory/secret.txt"_s).isEmpty(), "directory symlink cannot escape the resource root");
    std::filesystem::create_symlink(root / "missing", resources / "broken-link");
    check(resolve("broken-link"_s).isEmpty(), "dangling symlink fails closed");
    std::filesystem::create_symlink("loop-b", resources / "loop-a");
    std::filesystem::create_symlink("loop-a", resources / "loop-b");
    check(resolve("loop-a"_s).isEmpty(), "symlink loop fails closed");
    std::filesystem::create_directory_symlink(resources, root / "resource-alias");
    check(resolveWebExtensionResourceURLHaiku(directoryURL(root / "resource-alias"), "manifest.json"_s).fileSystemPath() == manifest,
        "resource directory alias retains the canonical boundary");

    check(resolveWebExtensionResourceURLHaiku(URL { "https://example.test/"_s }, "manifest.json"_s).isEmpty(), "non-file base URL is rejected");
    check(canonicalWebExtensionPathHaiku("relative/path"_s).isEmpty(), "relative native directory cannot depend on the working directory");
    check(canonicalWebExtensionPathHaiku(nativePath(root / "not-present")).isEmpty(), "canonicalization failure does not return the unchecked input");
    std::filesystem::remove_all(root);
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
