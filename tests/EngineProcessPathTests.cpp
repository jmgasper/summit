#include "config.h"
#include "ProcessExecutablePath.h"
#include <wtf/MainThread.h>
#include <wtf/text/CString.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    WTF::initializeMainThread();
    auto web = WebKit::executablePathOfWebProcess().utf8();
    auto network = WebKit::executablePathOfNetworkProcess().utf8();
    bool webMatches = !std::strcmp(web.legacyCStringPointer(), argv[1]);
    bool networkMatches = !std::strcmp(network.legacyCStringPointer(), argv[2]);
    std::printf("%s web-process path: %s\n", webMatches ? "PASS" : "FAIL", web.legacyCStringPointer());
    std::printf("%s network-process path: %s\n", networkMatches ? "PASS" : "FAIL", network.legacyCStringPointer());
    return webMatches && networkMatches ? 0 : 1;
}
