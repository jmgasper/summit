#include "config.h"
#include <Application.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>
#include <wtf/text/CString.h>

static int checks = 0, failures = 0;
static void Check(bool result, const char* label)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

static std::filesystem::path Create(const std::string& parent)
{
    RELEASE_ASSERT(!setenv("TMPDIR", parent.c_str(), 1));
    auto result = FileSystem::createTemporaryDirectory();
    return result.isNull() ? std::filesystem::path() : result.utf8().legacyCStringPointer();
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-temp-directory-tests");
    WTF::initializeMainThread();
    char ownedRoot[] = "/tmp/summit-temp-directory-XXXXXX";
    RELEASE_ASSERT(mkdtemp(ownedRoot));
    const std::filesystem::path root(ownedRoot);
    const auto parent = root / "parent";
    std::filesystem::create_directory(parent);
    const char* previous = getenv("TMPDIR");
    const bool hadPrevious = previous;
    const std::string previousValue = previous ? previous : "";
    auto previousMask = umask(0);

    auto first = Create(parent.string());
    Check(!first.empty() && first.parent_path() == parent,
        "temporary directory is inside a parent without a trailing separator");
    Check(!first.empty() && std::filesystem::is_directory(first),
        "successful result names an actual directory");
    struct stat info;
    Check(!first.empty() && !stat(first.c_str(), &info) && (info.st_mode & 0777) == 0700,
        "new temporary directory is private even with a permissive umask");

    auto trailing = Create(parent.string() + "/");
    Check(!trailing.empty() && trailing.parent_path() == parent && std::filesystem::is_directory(trailing),
        "parent with a trailing separator creates a contained directory");
    const auto unicodeParent = root / "space and 雪";
    std::filesystem::create_directory(unicodeParent);
    auto unicode = Create(unicodeParent.string());
    Check(!unicode.empty() && unicode.parent_path() == unicodeParent && std::filesystem::is_directory(unicode),
        "temporary parent names preserve spaces and Unicode");

    std::set<std::filesystem::path> paths;
    bool valid = true;
    for (unsigned i = 0; i < 20; ++i) {
        auto path = Create(parent.string());
        valid &= !path.empty() && path.parent_path() == parent && std::filesystem::is_directory(path);
        paths.insert(path);
    }
    Check(valid && paths.size() == 20, "repeated requests create twenty distinct contained directories");
    Check(Create((root / "missing").string()).empty(), "missing parent reports failure");
    Check(!std::filesystem::exists(root / "missing"), "failed request does not create its parent");
    const auto file = root / "existing-file";
    { std::ofstream output(file); output << "preserve this file"; }
    Check(Create(file.string()).empty(), "regular file cannot act as the temporary parent");
    { std::ifstream input(file); std::string content(std::istreambuf_iterator<char>(input), {});
      Check(content == "preserve this file", "failure preserves the existing file contents"); }

    umask(previousMask);
    if (hadPrevious) setenv("TMPDIR", previousValue.c_str(), 1);
    else unsetenv("TMPDIR");
    std::filesystem::remove_all(root);
    Check(!std::filesystem::exists(root), "all fixture directories and files are removed");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
