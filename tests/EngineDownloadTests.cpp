#include "DownloadFilename.h"
#include <cstdio>
#include <string>

int main()
{
    using namespace BPrivate;
    int checks = 0, failures = 0;
    auto check = [&](bool passed, const char* label) {
        ++checks;
        failures += !passed;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    };
    check(sanitizedDownloadFilename("report.pdf") == "report.pdf", "ordinary filename is preserved");
    check(sanitizedDownloadFilename("../../escape.txt") == "escape.txt", "parent-directory components are removed");
    check(sanitizedDownloadFilename("/boot/home/escape.txt") == "escape.txt", "absolute path cannot choose a destination directory");
    check(sanitizedDownloadFilename("C:\\folder\\escape.txt") == "escape.txt", "Windows path components are removed");
    check(sanitizedDownloadFilename("/folder\\another/escape.txt") == "escape.txt", "mixed path separators are removed");
    check(sanitizedDownloadFilename("..").Compare("Download") == 0, "parent directory name has a safe fallback");
    check(sanitizedDownloadFilename(".") == "Download", "current directory name has a safe fallback");
    check(sanitizedDownloadFilename("../") == "Download", "empty path leaf has a safe fallback");
    check(sanitizedDownloadFilename("") == "Download", "empty filename has a safe fallback");
    check(sanitizedDownloadFilename("a\nb\t.txt") == "a_b_.txt", "control characters are replaced");
    check(sanitizedDownloadFilename("café 雪.html") == "café 雪.html", "Unicode filename is preserved");
    std::string longName(241, 'x');
    longName += "雪.txt";
    check(sanitizedDownloadFilename(longName.c_str()) == std::string(241, 'x').c_str(), "truncation preserves UTF-8 character boundaries");
    auto bounded = sanitizedDownloadFilename(std::string(500, 'x').c_str());
    check(bounded.Length() == B_FILE_NAME_LENGTH - 1 - 12, "long filenames reserve room for uniqueness counters");
    check(downloadFilenameWithCounter(bounded, 4294967295U).Length() < B_FILE_NAME_LENGTH, "largest collision counter fits the filesystem limit");
    check(downloadFilenameWithCounter("café 雪.html", 12) == "café 雪-12.html", "Unicode collision name preserves the complete extension");
    check(downloadFilenameWithCounter("archive.tar.gz", 0) == "archive.tar-0.gz", "collision counter precedes the final extension");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
