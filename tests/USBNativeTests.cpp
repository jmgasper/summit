#include "USBDeviceSessionHaiku.h"
#include <cstdio>
using namespace WebKit::USBHaiku;

int main()
{
    unsigned found = 0;
    unsigned permitted = 0;
    for (auto& device : enumerateDevices()) {
        ++found;
        bool allowed = eligible(device);
        permitted += allowed;
        std::printf("%04x:%04x: %s (%zu configurations)\n", device.vendorId, device.productId,
            allowed ? "eligible" : "protected or invalid", device.configurations.size());
        if (!allowed) {
            Session session;
            if (session.open(device).error != Error::Security || session.opened()) {
                std::fprintf(stderr, "Protected device was not rejected before opening\n");
                return 1;
            }
        }
    }
    Session closed;
    if (closed.transfer(true, 1, 64).error != Error::InvalidState
        || closed.controlTransfer(true, 0, 0, 6, 0x100, 0, 18).error != Error::InvalidState
        || closed.claimInterface(0).error != Error::InvalidState
        || closed.selectConfiguration(1).error != Error::InvalidState
        || closed.reset().error != Error::InvalidState) {
        std::fprintf(stderr, "Closed-device guards failed\n");
        return 1;
    }
    if (describeDevice("/etc/passwd") || describeDevice("/dev/bus/usb/../x")) {
        std::fprintf(stderr, "Path confinement failed\n");
        return 1;
    }
    std::printf("Native USB enumeration: %u devices, %u eligible. Protected-device and closed-session checks passed.\n", found, permitted);
}
