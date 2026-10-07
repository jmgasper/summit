// air/OS adapter/scan/cancellation checks, with opt-in selected-device GATT.
#include "BluetoothSessionHaiku.h"
#include <Application.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace WebKit::BluetoothHaiku;
int main(int argc, char** argv)
{
    BApplication app("application/x-vnd.Summit-BluetoothTests");
    if (app.InitCheck() != B_OK) return 1;
    if (!available()) { std::puts("BLUETOOTH_NATIVE_UNAVAILABLE"); return 2; }
    std::atomic<bool> cancelled { true };
    std::vector<Device> devices;
    if (scan(devices, cancelled, 100) != NativeError::Aborted || !devices.empty()) return 1;
    cancelled = false;
    auto start = std::chrono::steady_clock::now();
    std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); cancelled = true; });
    auto result = scan(devices, cancelled, 8000);
    cancel.join();
    if (result != NativeError::Aborted || std::chrono::steady_clock::now() - start > std::chrono::seconds(6)) return 1;
    cancelled = false;
    if (scan(devices, cancelled, 4000) != NativeError::None) return 1;
    std::printf("BLUETOOTH_NATIVE_SCAN_PASS count=%zu cancellation=pass\n", devices.size());
    if (argc < 2) return 0;
    auto target = std::find_if(devices.begin(), devices.end(), [&](auto& device) { return device.advertisement.name == argv[1] && device.advertisement.connectable; });
    if (target == devices.end()) { std::puts("BLUETOOTH_TARGET_NOT_ADVERTISING"); return 0; }
    std::puts("BLUETOOTH_TARGET_FOUND");
    if (argc < 3 || std::string_view(argv[2]) != "--connect") return 0;
    Session session(*target);
    auto connected = session.connect();
    if (connected != NativeError::None) { std::printf("BLUETOOTH_CONNECT_FAILED code=%d\n", int(connected)); return 1; }
    std::vector<Service> services;
    auto status = session.client().services(services);
    if (!status) return 1;
    bool readBattery = false, readInformation = false;
    for (auto& service : services) {
        bool battery = service.uuid == UUID::alias(0x180f);
        bool information = service.uuid == UUID::alias(0x180a);
        if (!battery && !information) continue;
        std::vector<Characteristic> characteristics;
        if (!session.client().characteristics(service, characteristics)) return 1;
        for (auto& characteristic : characteristics) {
            if (!(characteristic.properties & 2) || blocked(characteristic.uuid, Access::Read)) continue;
            if (battery && characteristic.uuid != UUID::alias(0x2a19)) continue;
            if (information && characteristic.uuid != UUID::alias(0x2a29) && characteristic.uuid != UUID::alias(0x2a24)) continue;
            std::vector<uint8_t> bytes;
            auto read = session.client().read(characteristic.value, bytes);
            if (!read) { std::printf("BLUETOOTH_READ_FAILED error=%d remote=%u\n", int(read.error), read.remoteError); return 1; }
            readBattery |= battery;
            readInformation |= information;
            std::printf("BLUETOOTH_READ_PASS service=%s bytes=%zu\n", battery ? "battery" : "device_information", bytes.size());
        }
    }
    session.disconnect();
    if (session.connected()) return 1;
    std::printf("BLUETOOTH_GATT_PASS services=%zu battery=%d information=%d\n", services.size(), readBattery, readInformation);
    return 0;
}
