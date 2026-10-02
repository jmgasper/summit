#include "Location.h"
#include <Message.h>
#include <NetworkAddress.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <SecureSocket.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace summit {
namespace {
constexpr const char* kHost = "api.beacondb.net";

struct AccessPoint {
    std::string address;
    int signal = 0;
};

// The Wi-Fi networks the system last saw. Networks whose name ends in
// "_nomap" asked not to be used for location, as other services honour.
std::vector<AccessPoint> NearbyNetworks()
{
    std::vector<AccessPoint> found;
    BNetworkRoster& roster = BNetworkRoster::Default();
    BNetworkInterface interface;
    uint32 cookie = 0;
    while (roster.GetNextInterface(&cookie, interface) == B_OK) {
        BNetworkDevice device(interface.Name());
        if (!device.IsWireless()) continue;
        wireless_network* networks = nullptr;
        uint32 count = 0;
        if (device.GetNetworks(networks, count) != B_OK || count < 2) {
            delete[] networks;
            networks = nullptr;
            count = 0;
            // Only the networks seen lately: no forced scan, which would
            // interrupt the connection.
            device.Scan(true, false);
            if (device.GetNetworks(networks, count) != B_OK) {
                delete[] networks;
                continue;
            }
        }
        for (uint32 i = 0; i < count; ++i) {
            const auto& network = networks[i];
            const std::string name(network.name, strnlen(network.name, sizeof(network.name)));
            if (name.size() >= 6 && name.compare(name.size() - 6, 6, "_nomap") == 0) continue;
            const uint8* mac = network.address.LinkLevelAddress();
            if (!mac || network.address.LinkLevelAddressLength() != 6) continue;
            char text[18];
            std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            // Locally administered addresses (phones' hotspots, randomised
            // addresses) say nothing about where they are.
            if (mac[0] & 0x02) continue;
            found.push_back({text, network.signal_strength ? static_cast<int>(network.signal_strength) - 256 : 0});
        }
        delete[] networks;
    }
    return found;
}

// One HTTPS request; the certificate must be valid for the host.
bool Post(const std::string& body, std::string& response, std::string& error)
{
    BNetworkAddress address(kHost, 443);
    if (address.InitCheck() != B_OK) {
        error = "BeaconDB could not be found";
        return false;
    }
    BSecureSocket socket;
    if (socket.Connect(address, 10000000) != B_OK) {
        error = "BeaconDB could not be reached";
        return false;
    }
    // HTTP/1.0: the answer ends when the connection does.
    const std::string request = std::string("POST /v1/geolocate HTTP/1.0\r\nHost: ") + kHost
        + "\r\nUser-Agent: Summit (Haiku web browser)\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (socket.Write(request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
        error = "BeaconDB could not be reached";
        return false;
    }
    char buffer[4096];
    while (response.size() < 65536) {
        if (socket.WaitForReadable(10000000) != B_OK) break;
        const ssize_t count = socket.Read(buffer, sizeof(buffer));
        if (count <= 0) break;
        response.append(buffer, count);
    }
    if (response.rfind("HTTP/1.", 0) != 0) {
        error = "BeaconDB did not answer";
        return false;
    }
    const size_t space = response.find(' ');
    const int status = space == std::string::npos ? 0 : std::atoi(response.c_str() + space + 1);
    const size_t start = response.find("\r\n\r\n");
    if (start == std::string::npos) {
        error = "BeaconDB did not answer";
        return false;
    }
    response.erase(0, start + 4);
    if (status == 404) {
        error = "BeaconDB does not know where this network is";
        return false;
    }
    if (status != 200) {
        error = "BeaconDB answered with status " + std::to_string(status);
        return false;
    }
    return true;
}
}

void LookUpLocation(BMessenger reply, uint32 what)
{
    std::thread([reply, what] {
        BMessage result(what);
        try {
            nlohmann::json request = {{"considerIp", true}};
            const auto networks = NearbyNetworks();
            // A single network can not be placed; services want two at least.
            if (networks.size() >= 2) {
                auto list = nlohmann::json::array();
                for (const auto& network : networks) {
                    nlohmann::json entry = {{"macAddress", network.address}};
                    if (network.signal < 0 && network.signal > -120) entry["signalStrength"] = network.signal;
                    list.push_back(std::move(entry));
                }
                request["wifiAccessPoints"] = std::move(list);
            }
            std::string response, error;
            if (!Post(request.dump(), response, error)) {
                result.AddString("error", error.c_str());
            } else {
                const auto answer = nlohmann::json::parse(response);
                const auto& location = answer.at("location");
                result.AddDouble("latitude", location.at("lat").get<double>());
                result.AddDouble("longitude", location.at("lng").get<double>());
                result.AddDouble("accuracy", answer.value("accuracy", 25000.0));
                result.AddDouble("timestamp", static_cast<double>(std::time(nullptr)));
                result.AddInt32("networks", static_cast<int32>(networks.size()));
                if (answer.contains("fallback")) result.AddString("fallback", answer["fallback"].get<std::string>().c_str());
            }
        } catch (const std::exception&) {
            result.AddString("error", "BeaconDB's answer could not be read");
        }
        reply.SendMessage(&result);
    }).detach();
}
}
