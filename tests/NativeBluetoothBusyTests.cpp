// Explicit opt-in check that the mouse driver's existing LE link stays owned.
// Reads only the public status archive, never the bond/key store.
#include "BluetoothSessionHaiku.h"
#include <Application.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
using namespace WebKit::BluetoothHaiku;
static constexpr uint32 code(const char* s) { return uint32(s[0]) << 24 | uint32(s[1]) << 16 | uint32(s[2]) << 8 | uint32(s[3]); }
static bool inputStatus(Device& device, std::array<uint8_t, 6>& local, bigtime_t& updated)
{
    char settings[B_PATH_NAME_LENGTH];
    if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, settings, sizeof(settings)) != B_OK) return false;
    BFile file((std::string(settings) + "/bluetooth/le_status").c_str(), B_READ_ONLY);
    BMessage archive;
    system_info info;
    if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK || get_system_info(&info) != B_OK
        || archive.GetInt64("boot_time", 0) != info.boot_time) return false;
    BMessage entry; unsigned count = 0;
    for (int32 i = 0; archive.FindMessage("device", i, &entry) == B_OK; ++i) {
        if (!entry.GetBool("connected", false)) continue;
        const void* peer; const void* adapter; ssize_t size, localSize;
        if (entry.FindData("address", B_RAW_TYPE, &peer, &size) != B_OK || size != 6
            || entry.FindData("local", B_RAW_TYPE, &adapter, &localSize) != B_OK || localSize != 6) return false;
        std::memcpy(device.address.data(), peer, 6); std::memcpy(local.data(), adapter, 6);
        device.addressType = entry.GetUInt8("address_type", 0); updated = entry.GetInt64("updated", 0); ++count;
    }
    return count == 1;
}
int main(int argc, char** argv)
{
    BApplication app("application/x-vnd.Summit-BluetoothBusyTests");
    Device device; std::array<uint8_t, 6> local; bigtime_t updated;
    if (!inputStatus(device, local, updated)) { std::puts("BLUETOOTH_BUSY_SKIP no single connected input device"); return 2; }
    std::puts("BLUETOOTH_INPUT_CONNECTED status=read_only");
    if (argc != 2 || std::string_view(argv[1]) != "--test-busy-input") return 0;
    BMessenger server("application/x-vnd.Haiku-bluetooth_server");
    BMessage acquire(code("btAd")), reply;
    BMessage count(code("btCd")), counted;
    if (server.SendMessage(&count, &counted, 1000000, 1000000) != B_OK || counted.GetInt32("count", 0) != 1) {
        std::puts("BLUETOOTH_BUSY_SKIP adapter mapping is ambiguous"); return 2;
    }
    if (server.SendMessage(&acquire, &reply, 1000000, 1000000) != B_OK || reply.GetInt32("status", B_ERROR) != B_OK) return 1;
    device.adapter = reply.GetInt32("hci_id", -1);
    if (device.adapter < 0) return 1;
    { Session session(device); auto result = session.connect();
        std::printf("BLUETOOTH_BUSY_RESULT code=%d\n", int(result));
        if (result != NativeError::Busy || session.connected()) return 1; }
    snooze(300000);
    Device after; std::array<uint8_t, 6> afterLocal; bigtime_t afterUpdated;
    if (!inputStatus(after, afterLocal, afterUpdated) || !device.samePeer(after) || local != afterLocal || updated != afterUpdated) return 1;
    std::puts("BLUETOOTH_NATIVE_BUSY_PASS input_link=preserved session=closed");
    return 0;
}
