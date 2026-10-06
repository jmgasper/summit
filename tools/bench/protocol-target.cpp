// Native test recipient for ProtocolHandlers. Registers only its own test
// scheme and records the exact argv delivered by BRoster. --remove cleans up.
#include <AppFileInfo.h>
#include <Application.h>
#include <Entry.h>
#include <File.h>
#include <Mime.h>
#include <Path.h>
#include <Roster.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* signature = "application/x-vnd.Summit.Bench.ProtocolTarget";
constexpr const char* schemeType = "application/x-vnd.Be.URL.web+summitnative";
constexpr const char* output = "/boot/home/summit/bench/protocol-target-argv.bin";

class Target final : public BApplication {
public:
    Target() : BApplication(signature) { }
    void ArgvReceived(int32 count, char** values) override
    {
        // Length prefixes make newline/quote/Unicode argument changes visible.
        if (FILE* file = std::fopen(output, "wb")) {
            std::fprintf(file, "%ld\n", long(count));
            for (int32 i = 0; i < count; ++i) {
                const size_t length = std::strlen(values[i]);
                std::fprintf(file, "%zu\n", length);
                std::fwrite(values[i], 1, length, file);
                std::fputc('\n', file);
            }
            std::fclose(file);
        }
    }
    void ReadyToRun() override { PostMessage(B_QUIT_REQUESTED); }
};
}

int main(int argc, char** argv)
{
    if (argc == 2 && (!std::strcmp(argv[1], "--install") || !std::strcmp(argv[1], "--remove"))) {
        BEntry entry(argv[0], true);
        entry_ref ref;
        if (entry.GetRef(&ref) != B_OK) return 1;
        BMimeType application(signature), scheme(schemeType);
        entry_ref previous;
        if (application.IsInstalled() && (application.GetAppHint(&previous) != B_OK || previous != ref)) {
            std::fputs("The test signature belongs to another file; leaving it alone.\n", stderr);
            return 1;
        }
        char preferred[B_MIME_TYPE_LENGTH] = "";
        if (scheme.IsInstalled() && (scheme.GetPreferredApp(preferred, B_OPEN) != B_OK || std::strcmp(preferred, signature))) {
            std::fputs("The test scheme belongs to another application; leaving it alone.\n", stderr);
            return 1;
        }
        if (!std::strcmp(argv[1], "--remove")) {
            scheme.Delete(); application.Delete();
            return 0;
        }
        if (scheme.IsInstalled() && !application.IsInstalled()) {
            std::fputs("The test scheme already exists; leaving it alone.\n", stderr);
            return 1;
        }
        BFile file(&entry, B_READ_WRITE);
        BAppFileInfo info(&file);
        if (info.SetSignature(signature) != B_OK || info.SetAppFlags(B_MULTIPLE_LAUNCH) != B_OK
            || application.Install() != B_OK || application.SetAppHint(&ref) != B_OK
            || scheme.Install() != B_OK || scheme.SetPreferredApp(signature, B_OPEN) != B_OK)
            return 1;
        std::puts("Registered web+summitnative with the test recipient.");
        return 0;
    }
    Target application;
    application.Run();
}
