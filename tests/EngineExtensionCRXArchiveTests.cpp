#include "config.h"
#include "WebExtensionArchiveHaiku.h"
#include <Application.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <wtf/MainThread.h>

static unsigned checks, failures;
static void check(bool result, const std::string& message)
{
    ++checks; failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", message.c_str());
}
static std::string digest(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::string bytes { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    std::array<unsigned char, EVP_MAX_MD_SIZE> result; unsigned length = 0;
    if (!stream || EVP_Digest(bytes.data(), bytes.size(), result.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32) return { };
    std::string text;
    for (unsigned i = 0; i < length; ++i) { text += "0123456789abcdef"[result[i] >> 4]; text += "0123456789abcdef"[result[i] & 15]; }
    return text;
}
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    BApplication app("application/x-vnd.Kunanyi-Summit-crx-archive-tests");
    WTF::initializeMainThread();
    const auto fixtures = std::filesystem::path(argv[1]);
    const auto temporary = std::filesystem::path(argv[2]);
    std::ifstream expectedFile(fixtures / "expected.json");
    const auto expected = nlohmann::json::parse(expectedFile);
    const auto output = temporary / "extracted";
    std::filesystem::create_directory(output);
    for (const auto& [name, expectation] : expected.items()) {
        if (digest(fixtures / name) != expectation.at("sha256").get<std::string>()) { check(false, name + " frozen fixture"); continue; }
        {
            auto result = WebKit::extractWebExtensionArchiveHaiku(String::fromUTF8((fixtures / name).string()), { }, String::fromUTF8(output.string()));
            const bool success = expectation.at("valid").get<bool>() && expectation.value("extract", false);
            check(result.has_value() == success, name + " extraction outcome");
            if (result && success) {
                check(std::string(result->verifiedCRXIdentifier().utf8().legacyCStringPointer()) == expectation.at("identifier").get<std::string>(), name + " signer metadata");
                const auto path = std::filesystem::path(result->path().utf8().legacyCStringPointer());
                auto actual = nlohmann::json::object(); bool privateModes = true;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                    struct stat status { }; if (lstat(entry.path().c_str(), &status)) { privateModes = false; continue; }
                    if (std::filesystem::is_regular_file(entry.symlink_status())) {
                        actual[entry.path().lexically_relative(path).generic_string()] = digest(entry.path());
                        privateModes &= (status.st_mode & 07777) == 0600 && status.st_nlink == 1;
                    } else privateModes &= std::filesystem::is_directory(entry.symlink_status()) && (status.st_mode & 07777) == 0700;
                }
                check(actual == expectation.at("files_sha256"), name + " all authenticated resources preserved");
                check(privateModes, name + " private regular resources");
                auto moved = std::move(*result);
                check(result->path().isEmpty() && result->verifiedCRXIdentifier().isEmpty()
                    && !moved.verifiedCRXIdentifier().isEmpty(), name + " signer metadata follows ownership");
            }
        }
        check(std::filesystem::is_empty(output), name + " automatic cleanup");
        check(!std::filesystem::exists(temporary / "escape.txt") && !std::filesystem::exists(output / "escape.txt"), name + " no traversal escape");
    }
    WebKit::WebExtensionArchiveLimitsHaiku limits; limits.archiveBytes = 1;
    auto limited = WebKit::extractWebExtensionArchiveHaiku(String::fromUTF8((fixtures / "rsa.crx").string()), limits, String::fromUTF8(output.string()));
    check(!limited && limited.error() == WebKit::WebExtensionArchiveErrorHaiku::ExpansionLimit, "CRX compressed input bound");
    check(std::filesystem::is_empty(output), "bounded CRX leaves no output");
    std::filesystem::remove(output);
    std::printf("CRX_ARCHIVE_RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
