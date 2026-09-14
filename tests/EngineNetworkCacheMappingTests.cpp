#include "config.h"
#include "NetworkCacheData.h"
#include "SharedMemory.h"
#include <OS.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
static unsigned failures;
static void check(bool value, const char* message)
{
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

int main()
{
    using namespace WebKit::NetworkCache;
    using Memory = WebCore::SharedMemory;
    using Protection = Memory::Protection;
    check(!Data().tryCreateSharedMemory(), "null cache data has no mapping");
    check(!Data::empty().tryCreateSharedMemory(), "empty cache data has no mapping");
    std::array<uint8_t, 8193> bytes;
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(i * 37);
    check(!Data(std::span<const uint8_t> { bytes }).tryCreateSharedMemory(), "buffer-backed cache data is not treated as a file mapping");

    char path[] = "/tmp/summit-cache-map-XXXXXX";
    int writer = mkstemp(path);
    if (writer < 0)
        return 2;
    auto written = write(writer, bytes.data(), bytes.size());
    close(writer);
    if (written != static_cast<ssize_t>(bytes.size())) {
        unlink(path);
        return 2;
    }
    int temporary = open(path, O_RDONLY | O_CLOEXEC);
    void* borrowedMap = temporary < 0 ? MAP_FAILED : mmap(nullptr, bytes.size(), PROT_READ, MAP_PRIVATE, temporary, 0);
    if (temporary >= 0)
        close(temporary);
    if (borrowedMap != MAP_FAILED) {
        auto invalidWrapper = Memory::wrapMap(borrowedMap, bytes.size(), temporary);
        check(!invalidWrapper, "wrapping fails if its descriptor cannot be duplicated");
        check(area_for(borrowedMap) >= 0, "failed wrapping preserves the borrowed mapping");
        munmap(borrowedMap, bytes.size());
    } else
        check(false, "create borrowed mapping for descriptor-failure check");
    auto data = mapFile(String::fromUTF8(path));
    check(!data.isNull() && data.isMap() && data.size() == bytes.size(), "cache file is mapped at its exact length");
    auto retained = data;
    data = { };
    auto shared = retained.tryCreateSharedMemory();
    check(!!shared, "copied cache data retains its mapped-file descriptor");
    if (!shared) {
        unlink(path);
        return 1;
    }
    check(shared->size() == bytes.size() && !std::memcmp(shared->span().data(), bytes.data(), bytes.size()), "wrapper exposes all mapped bytes");
    auto handle = shared->createHandle(Protection::ReadOnly);
    check(!!handle, "wrapped read-only file exports a read-only capability");
    check(!shared->createHandle(Protection::ReadWrite), "read-only cache mapping cannot export a writable capability");
    auto receiver = handle ? Memory::map(WTF::move(*handle), Protection::ReadOnly) : nullptr;
    check(receiver && !std::memcmp(receiver->span().data(), bytes.data(), bytes.size()), "receiver sees complete cached content");
    if (receiver) {
        area_info info;
        check(get_area_info(area_for(receiver->mutableSpan().data()), &info) == B_OK && !(info.protection & B_WRITE_AREA), "receiver mapping is enforced read-only by Haiku");
        check(!receiver->createHandle(Protection::ReadWrite), "receiver cannot upgrade a cache capability");
    }
    check(unlink(path) == 0, "cache file can be unlinked while mappings are retained");
    auto secondHandle = shared->createHandle(Protection::ReadOnly);
    check(!!secondHandle, "unlinked mapping can still export a descriptor");
    // The wrapper borrows its address from Data. Export before releasing that
    // owner, then verify the independent descriptor mapping survives both.
    retained = { };
    shared = nullptr;
    auto afterRelease = secondHandle ? Memory::map(WTF::move(*secondHandle), Protection::ReadOnly) : nullptr;
    check(afterRelease && !std::memcmp(afterRelease->span().data(), bytes.data(), bytes.size()), "exported descriptor survives cache-data and wrapper destruction");
    check(receiver && !std::memcmp(receiver->span().data(), bytes.data(), bytes.size()), "existing receiver mapping survives original owner destruction");
    std::printf("%u native cache mapping checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
