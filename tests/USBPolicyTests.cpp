#include "USBDevicePolicyHaiku.h"
#include <cstdio>
#include <limits>
using namespace WebKit::USBHaiku;

static unsigned checks;
static unsigned failures;
static void check(bool condition, const char* name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

int main()
{
    Device d;
    d.vendorId = 0x2341;
    d.productId = 0x0043;
    d.serial = "board-1";
    d.configurations = { Configuration { 1, "", { Interface { 7, 0, false, { Alternate { 0, 0xff, 3, 5, "", { Endpoint { 2, true, 2, 64 } } } } } } } };
    check(eligible(d), "vendor device is eligible");
    check(matches(d, Filter { }), "empty filter matches");
    Filter f;
    f.productId = d.productId;
    check(!validFilter(f), "product requires vendor");
    f.vendorId = d.vendorId;
    check(validFilter(f) && matches(d, f), "vendor/product pair");
    f.serialNumber = "different";
    check(!matches(d, f), "serial mismatch");
    f.serialNumber = d.serial;
    check(matches(d, f), "exact serial");
    f.classCode = 0xff;
    f.subclassCode = 3;
    f.protocolCode = 5;
    check(matches(d, f), "alternate interface class tuple");
    f.protocolCode = 4;
    check(!matches(d, f), "protocol mismatch");
    check(matches(d, std::vector<Filter> { }, { }), "empty include list");
    check(!matches(d, std::vector<Filter> { }, { Filter { } }), "empty exclusion filter excludes all");
    f = { };
    f.protocolCode = 1;
    check(!validFilter(f), "protocol requires subclass");
    f = { };
    f.subclassCode = 1;
    check(!validFilter(f), "subclass requires class");
    for (auto c : { 1, 3, 8, 9, 11, 14, 16, 224 }) {
        auto protectedDevice = d;
        protectedDevice.configurations[0].interfaces[0].alternates.push_back(Alternate { 1, uint8_t(c), 0, 0, "", { } });
        check(!eligible(protectedDevice), "protected alternate blocks composite device");
        protectedDevice = d;
        protectedDevice.classCode = c;
        check(!eligible(protectedDevice), "protected device class");
    }
    check(blocklisted(0x1050, 0x0407, 0xffff), "Yubikey blocklist");
    check(blocklisted(0x18d1, 0x5026, 0x0100), "Titan blocklist");
    check(blocklisted(0x2ccf, 0x0880, 0x0100), "last blocklist entry");
    check(!blocklisted(0x2341, 0x0043, 0x0100), "board not blocklisted");
    for (unsigned request = 0; request < 256; ++request) {
        check(!allowedStandardControl(false, request), "standard OUT is forbidden");
        bool readOnly = request == 0 || request == 6 || request == 8 || request == 10 || request == 12;
        check(allowedStandardControl(true, request) == readOnly, "standard IN allowlist");
    }
    size_t total;
    check(validPacketLengths({ 0, 64, 32 }, total) && total == 96, "packet total");
    check(!validPacketLengths({ 32768 }, total), "native signed packet bound");
    check(!validPacketLengths({ std::numeric_limits<uint32_t>::max() }, total), "packet overflow");
    check(!validPacketLengths(std::vector<uint32_t>(1025, 1), total), "packet count bound");
    check(!validPacketLengths(std::vector<uint32_t>(1024, 32767), total), "aggregate transfer bound");
    check(validPacketLengths({ }, total) && !total, "zero packets");
    std::printf("USB policy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
