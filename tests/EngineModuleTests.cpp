#include "config.h"
#include "Module.h"
#include <cstdio>
#include <cstring>
#include <wtf/MainThread.h>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}
static int LoadedCount(const char* path)
{
    image_info info;
    int32 cookie = 0;
    int count = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
        count += !std::strcmp(info.name, path);
    return count;
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    WTF::initializeMainThread();
    using Answer = int (*)();
    {
        WebKit::Module missing("/nonexistent/summit-module-fixture.so"_s);
        Check(!missing.load(), "missing native module reports load failure");
        Check(!missing.functionPointer<Answer>("SummitModuleAnswer"), "failed load cannot resolve a symbol");
        missing.unload();
        missing.unload();
    }
    { WebKit::Module unopened(String::fromUTF8(argv[1])); }
    Check(!LoadedCount(argv[1]), "destroying an unopened module leaves its image unloaded");
    {
        WebKit::Module module(String::fromUTF8(argv[1]));
        Check(module.load() && LoadedCount(argv[1]) == 1, "native module loads from a path containing spaces and Unicode");
        auto answer = module.functionPointer<Answer>("SummitModuleAnswer");
        Check(answer && answer() == 42, "resolved native symbol executes the fixture function");
        Check(!module.functionPointer<Answer>("NoSuchSummitSymbol"), "missing symbol returns null");
        bool repeated = true;
        for (int i = 0; i < 10; ++i) repeated &= module.load();
        module.unload();
        Check(repeated && !LoadedCount(argv[1]), "repeated load calls do not retain extra loader references");
        Check(!module.functionPointer<Answer>("SummitModuleAnswer"), "unloaded module cannot return stale function pointers");
        Check(module.load(), "module can load again after explicit unload");
    }
    Check(!LoadedCount(argv[1]), "module destruction releases its loaded image");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
