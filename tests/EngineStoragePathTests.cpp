#include "config.h"
#include "StoragePathsHaiku.h"
#include <Application.h>
#include <FindDirectory.h>
#include <Path.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/MainThread.h>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
static std::string UTF8(const String& value) { return value.utf8().legacyCStringPointer(); }

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-storage-tests");
    WTF::initializeMainThread();
    char directory[] = "/tmp/summit-storage-XXXXXX";
    RELEASE_ASSERT(mkdtemp(directory));
    auto root = std::filesystem::path(directory);
    auto profileA = root / "profile A 雪";
    auto profileB = root / "profile B";
    auto baseA = String::fromUTF8(profileA.c_str());
    auto baseB = String::fromUTF8(profileB.c_str());
    using WebKit::storageDirectoryPathHaiku;
    auto path = storageDirectoryPathHaiku(false, baseA, "IndexedDB"_s, false);
    Check(UTF8(path) == (profileA / "IndexedDB").string(), "explicit profile base preserves spaces and Unicode");
    Check(!std::filesystem::exists(profileA), "path discovery does not create directories when disabled");
    Check(storageDirectoryPathHaiku(false, baseA, "IndexedDB"_s, true) == path
        && std::filesystem::is_directory(profileA / "IndexedDB"), "requested data directory is created");
    struct stat info;
    Check(!stat(profileA.c_str(), &info) && (info.st_mode & 0777) == 0700,
        "new profile directories grant access only to their owner");
    auto second = storageDirectoryPathHaiku(false, baseB, "IndexedDB"_s, true);
    Check(second != path && UTF8(second) == (profileB / "IndexedDB").string(),
        "separate profile bases resolve to separate storage directories");
    Check(storageDirectoryPathHaiku(false, ""_s, "IndexedDB"_s, true).isNull(),
        "explicitly empty base cannot create storage in the working directory");
    Check(storageDirectoryPathHaiku(false, "relative-profile"_s, "IndexedDB"_s, true).isNull(),
        "relative profile base is rejected");
    Check(storageDirectoryPathHaiku(false, String::fromUTF8(std::span { "/tmp\0elsewhere", 14 }), "IndexedDB"_s, true).isNull(),
        "embedded NUL cannot truncate the profile base");
    Check(storageDirectoryPathHaiku(false, baseA, String::fromUTF8(std::span { "a\0b", 3 }), true).isNull(),
        "embedded NUL cannot truncate a storage child");
    Check(storageDirectoryPathHaiku(false, baseA, "/absolute-child"_s, true).isNull(),
        "absolute storage child is rejected");
    Check(storageDirectoryPathHaiku(false, baseA, "nested/../../escape"_s, true).isNull(),
        "parent traversal in a storage child is rejected");
    Check(storageDirectoryPathHaiku(false, baseA, ""_s, false) == baseA,
        "empty storage child selects the explicit base itself");
    auto unavailable = root / "unavailable";
    { std::ofstream file(unavailable); file << "preserve this file"; }
    Check(storageDirectoryPathHaiku(false, String::fromUTF8(unavailable.c_str()), "IndexedDB"_s, true).isNull(),
        "unavailable destination reports failure");
    { std::ifstream file(unavailable); std::string data((std::istreambuf_iterator<char>(file)), {});
      Check(data == "preserve this file", "directory creation failure preserves the existing file"); }

    auto cache = storageDirectoryPathHaiku(true, nullString(), "NativePathProbe"_s, false);
    auto data = storageDirectoryPathHaiku(false, nullString(), "NativePathProbe"_s, false);
    BPath cacheBase, dataBase;
    RELEASE_ASSERT(find_directory(B_USER_CACHE_DIRECTORY, &cacheBase) == B_OK);
    RELEASE_ASSERT(find_directory(B_USER_SETTINGS_DIRECTORY, &dataBase) == B_OK);
    const auto cachePrefix = std::string(cacheBase.Path()) + "/WebKit/";
    const auto dataPrefix = std::string(dataBase.Path()) + "/WebKit/";
    Check(UTF8(cache).starts_with(cachePrefix), "default cache uses the native user cache location");
    Check(UTF8(data).starts_with(dataPrefix), "default persistent data uses native user settings");
    Check(cache != data, "default cache and persistent data directories are distinct");
    auto tail = UTF8(data).substr(dataPrefix.size());
    Check(tail.find("Kunanyi-Summit-storage-tests") != std::string::npos
        && tail.find('/') == tail.rfind('/'), "application signature forms one escaped namespace component");
    std::filesystem::remove_all(root);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
