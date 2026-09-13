#include <Application.h>
#include <cstdio>
#include <cstring>

// An invalid MIME signature fails before registrar registration or app_server
// connection. Each mode runs in its own process; no existing app is contacted.
int main(int argc, char** argv)
{
    if (argc != 2)
        return 64;
    constexpr const char* invalidSignature = "invalid";
    if (!std::strcmp(argv[1], "baseline")) {
        std::puts("BASELINE before constructor");
        std::fflush(stdout);
        BApplication application(invalidSignature);
        std::puts("BASELINE unexpectedly returned");
        return 65;
    }
    if (!std::strcmp(argv[1], "checked")) {
        status_t status = B_OK;
        BApplication application(invalidSignature, &status);
        if (status == B_OK || application.InitCheck() != status || be_app) {
            std::puts("FAIL invalid application initialization was accepted");
            return 66;
        }
        std::printf("CHECKED reported native failure %ld\n", static_cast<long>(status));
        // Match an auxiliary process entry point rejecting platformInitialize.
        return 1;
    }
    return 64;
}
