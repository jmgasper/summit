#include "config.h"
#include "WebExtensionStateHaiku.h"

#include <Application.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/MainThread.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
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

static Ref<JSON::Object> state(unsigned generation)
{
    Ref result = JSON::Object::create();
    result->setString("first"_s, String::number(generation));
    result->setString("second"_s, String::number(generation));
    result->setString("name"_s, String::fromUTF8("雪 extension %25"));
    auto payload = std::string(16384, generation % 2 ? 'A' : 'B');
    result->setString("payload"_s, String::fromUTF8(payload.c_str()));
    Ref levels = JSON::Object::create();
    levels->setString("session"_s, "TRUSTED_CONTEXTS"_s);
    result->setObject("StorageAccessLevels"_s, WTF::move(levels));
    return result;
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-state-tests");
    WTF::initializeMainThread();
    char temporary[] = "/tmp/summit-extension-state-XXXXXX";
    if (!mkdtemp(temporary))
        return 2;
    const auto root = std::filesystem::path(temporary);
    const auto path = root / "State %25 雪.json";
    auto read = [&] { return readWebExtensionStateHaiku(nativePath(path)); };
    auto write = [&](unsigned generation) { return writeWebExtensionStateHaiku(nativePath(path), state(generation)); };
    auto noTemporaryFiles = [&] {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.path().filename().string().starts_with("State-"))
                return false;
        }
        return true;
    };

    auto missing = read();
    check(missing && missing.value()->size() == 0, "missing state produces an empty object");
    check(!std::filesystem::exists(path), "reading missing state does not create a file");
    check(write(0).has_value(), "initial state is written");
    auto loaded = read();
    check(loaded && loaded.value()->getString("name"_s) == String::fromUTF8("雪 extension %25"), "Unicode state and filenames round-trip");
    RefPtr levels = loaded ? loaded.value()->getObject("StorageAccessLevels"_s) : nullptr;
    check(levels && levels->getString("session"_s) == "TRUSTED_CONTEXTS"_s,
        "nested access-level state survives serialization");
    struct stat info;
    check(!stat(path.c_str(), &info) && (info.st_mode & 0777) == 0600, "state is private to the owning user");
    check(write(1).has_value(), "existing state is replaced");
    loaded = read();
    check(loaded && loaded.value()->getString("first"_s) == "1"_s, "replacement exposes the new state");
    check(noTemporaryFiles(), "successful replacement leaves no temporary files");

    for (const auto& invalid : { "", "{\"truncated\":", "[]", "null", "false" }) {
        { std::ofstream stream(path); stream << invalid; }
        auto result = read();
        check(!result && result.error() == WebExtensionStateErrorHaiku::InvalidJSON,
            "invalid or non-object state is reported as invalid JSON");
    }
    { std::ofstream stream(path, std::ios::binary); stream << "{\"bad\":\""; stream.put(char(0xff)); stream << "\"}"; }
    auto invalidUTF8 = read();
    check(!invalidUTF8 && invalidUTF8.error() == WebExtensionStateErrorHaiku::InvalidJSON, "invalid UTF-8 state is rejected");
    check(write(2).has_value(), "valid state can replace a corrupt file");

    auto absentParent = writeWebExtensionStateHaiku(nativePath(root / "absent/State.json"), state(3));
    check(!absentParent && absentParent.error() == WebExtensionStateErrorHaiku::TemporaryFileFailed,
        "write failure is reported when the state directory is absent");
    check(!std::filesystem::exists(root / "absent"), "state writing does not create an unrequested profile directory");
    const auto blocked = root / "blocked";
    std::filesystem::create_directory(blocked);
    { std::ofstream stream(blocked / "keep"); stream << "old"; }
    auto replacement = writeWebExtensionStateHaiku(nativePath(blocked), state(4));
    check(!replacement && replacement.error() == WebExtensionStateErrorHaiku::ReplaceFailed, "failed rename is reported");
    std::string original;
    { std::ifstream stream(blocked / "keep"); stream >> original; }
    check(original == "old", "failed replacement preserves the existing destination");
    check(noTemporaryFiles(), "failed replacement removes its temporary file");
    auto directoryRead = readWebExtensionStateHaiku(nativePath(blocked));
    check(!directoryRead && directoryRead.error() == WebExtensionStateErrorHaiku::ReadFailed, "a directory is not accepted as state");

    const auto protectedPath = root / "protected.json";
    check(writeWebExtensionStateHaiku(nativePath(protectedPath), state(5)).has_value(), "symlink target fixture is written");
    std::filesystem::remove(path);
    std::filesystem::create_symlink(protectedPath, path);
    check(write(6).has_value() && !std::filesystem::is_symlink(path), "replacement creates a file in place of a destination symlink");
    auto protectedState = readWebExtensionStateHaiku(nativePath(protectedPath));
    check(protectedState && protectedState.value()->getString("first"_s) == "5"_s, "destination symlink target remains unchanged");

    String nulPath = makeString(nativePath(path), static_cast<UChar>(0), "ignored"_s);
    auto badRead = readWebExtensionStateHaiku(nulPath);
    auto badWrite = writeWebExtensionStateHaiku(nulPath, state(7));
    check(!badRead && badRead.error() == WebExtensionStateErrorHaiku::InvalidPath, "embedded NUL cannot truncate a read path");
    check(!badWrite && badWrite.error() == WebExtensionStateErrorHaiku::InvalidPath, "embedded NUL cannot truncate a write path");
    check(!readWebExtensionStateHaiku("relative.json"_s), "state reads require an absolute profile path");
    check(!writeWebExtensionStateHaiku("relative.json"_s, state(7)), "state writes require an absolute profile path");

    std::atomic<bool> done { false };
    std::atomic<unsigned> reads { 0 }, inconsistentReads { 0 };
    Ref reader = WTF::Thread::create("Extension state reader"_s, [&] {
        do {
            auto current = read();
            if (!current || current.value()->getString("first"_s) != current.value()->getString("second"_s)
                || current.value()->getString("payload"_s).length() != 16384)
                ++inconsistentReads;
            ++reads;
        } while (!done.load());
    });
    unsigned writeFailures = 0;
    for (unsigned index = 10; index < 110; ++index)
        writeFailures += !write(index);
    done = true;
    reader->waitForCompletion();
    check(!writeFailures, "100 concurrent replacements complete successfully");
    check(reads > 1, "reader observes state during replacements");
    check(!inconsistentReads, "concurrent reads see complete state objects");
    check(noTemporaryFiles(), "concurrent replacements leave no temporary files");
    std::printf("CONCURRENT_READS %u\n", reads.load());
    std::filesystem::remove_all(root);
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
